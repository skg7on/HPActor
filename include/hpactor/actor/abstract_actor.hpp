// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include <hpactor/cli/cli_types.hpp>
#include <hpactor/mailbox/disruptor_mailbox_interface.hpp>
#include <hpactor/mailbox/mailbox_kind.hpp>
#include <hpactor/msg/typed_message.hpp>
#include <hpactor/ref/actor_address.hpp>
#include <hpactor/sched/dispatch_policy.hpp>
#include <hpactor/types/types.hpp>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace hpactor {

// Forward declarations
class ActorContext;
class ActorSystem;
namespace net {
enum class OpType : uint32_t;
} // namespace net

class LifecycleActor;

namespace sched {
class IScheduler;
} // namespace sched
namespace mailbox {
template <typename T> class MPSCActorMailbox;
} // namespace mailbox
namespace mem {
class Hibernatable;
} // namespace mem
class IDurableActor;

/// \brief Polymorphic base class for all actor types.
///
/// Owns identity, linking/monitoring, lifecycle queries, and the virtual
/// interface that the scheduler, transport, and supervision layers call.
/// Subclasses define how messages are received and dispatched.
///
/// \note Thread safety: Identity fields (id, type, address) are set once
///       during spawn and read-only thereafter. Linking and monitoring
///       methods delegate to \c ActorContext which synchronizes internally.
class AbstractActor : public std::enable_shared_from_this<AbstractActor> {
  public:
    virtual ~AbstractActor() = default;

    /// \brief Globally-unique actor identifier assigned at spawn.
    ActorId id() const {
        return id_;
    }
    /// \brief Pointer to the actor's \c ActorId for consumers that need
    ///        an address without copying.
    const ActorId* id_ptr() const {
        return &id_;
    }
    /// \brief Actor type tag.
    ActorType type() const {
        return type_;
    }
    /// \brief Full network-addressable identity.
    ActorAddress address() const {
        return address_;
    }
    /// \brief Owning \c ActorSystem reference.
    ActorSystem& system() {
        return system_;
    }
    const ActorSystem& system() const {
        return system_;
    }

    /// \brief Set actor address. Called by \c ActorSystem during spawn.
    void set_address(ActorAddress addr) {
        address_ = addr;
        id_ = addr.id;
        type_ = addr.type;
    }

    /// \brief Set scheduler back-reference. Called by \c ActorSystem during
    /// spawn.
    virtual void set_scheduler(sched::IScheduler* scheduler);
    /// \brief Set mailbox back-reference. Called by \c ActorSystem during
    /// spawn.
    virtual void set_mailbox(mailbox::MPSCActorMailbox<TypedMessage>* mailbox);

    /// \brief Establish bidirectional death sharing with \p other.
    ///
    /// When either actor terminates, the other receives a \c DownMsg.
    /// \param[in] other Target actor address.
    void link_to(const ActorAddr& other);
    /// \brief Remove a previously established link.
    /// \param[in] other Target actor address.
    void unlink_from(const ActorAddr& other);

    /// \brief Register one-way death monitoring of \p target.
    ///
    /// When \p target terminates this actor receives a \c DownMsg.
    /// \param[in] target Actor to monitor.
    void monitor(const ActorAddr& target);
    /// \brief Cancel a previously established monitor.
    /// \param[in] target Actor to stop monitoring.
    void demonitor(const ActorAddr& target);

    /// \brief Receive and process a message.
    ///
    /// Called by the scheduler on the actor's assigned worker thread.
    /// \param[in,out] msg The incoming typed message.
    /// \note Thread safety: Called from a single scheduler thread at a time.
    ///       Implementations must not block the calling thread.
    virtual void receive(TypedMessage& msg) = 0;

    /// \brief RTTI-free query for \c EventBasedActor subclasses.
    virtual bool is_event_based_actor() const {
        return false;
    }

    /// \brief RTTI-free downcast to the \c LifecycleActor mixin.
    ///
    /// Returns \c nullptr for actors that do not opt into lifecycle management.
    virtual LifecycleActor* as_lifecycle() {
        return nullptr;
    }
    virtual const LifecycleActor* as_lifecycle() const {
        return nullptr;
    }

    /// \brief RTTI-free context binding capability.
    ///
    /// Called by \c ActorSpawner during adoption to bind an execution context
    /// to this actor. Returns \c false by default; \c LocalActor overrides
    /// to accept the context through its existing storage.
    virtual bool bind_context(ActorContext* context) noexcept;

    /// \brief Post-construction activation hook.
    ///
    /// Called by \c ActorSpawner after context binding and before dispatch
    /// registration. \c LocalActor delegates to the existing \c on_activate().
    /// The default does nothing; successful adoption requires
    /// \c bind_context() returning \c true first.
    virtual void activate_after_spawn();

    /// \brief RTTI-free downcast to \c IDurableActor.
    ///
    /// Returns \c nullptr for actors that do not implement durable state.
    virtual class IDurableActor* as_durable() {
        return nullptr;
    }
    virtual const class IDurableActor* as_durable() const {
        return nullptr;
    }

    /// \brief RTTI-free downcast to \c mem::Hibernatable.
    ///
    /// Returns \c nullptr for actors that do not support memory-only
    /// hibernation.
    virtual class mem::Hibernatable* as_hibernatable() {
        return nullptr;
    }
    virtual const class mem::Hibernatable* as_hibernatable() const {
        return nullptr;
    }

    /// \brief Returns \c true for system actors that drain last during node
    ///        shutdown. \c MetricsActor, \c CliActor, and \c SpawnReceiver
    ///        override this.
    virtual bool is_system_actor() const {
        return false;
    }

    /// \brief Dispatch policy telling the scheduler how to execute this actor.
    ///
    /// \return \c Cooperative (default, M:N work-stealing pool).
    virtual sched::DispatchPolicy dispatch_policy() const {
        return sched::DispatchPolicy::Cooperative;
    }
    /// \brief Dispatch hints (CPU affinity, pool size) for dedicated actors.
    virtual sched::DispatchHints dispatch_hints() const {
        return {};
    }

    /// \brief The mailbox backend kind for this actor.
    ///
    /// Default is \c VariableMpsc. Fixed-mailbox actors override to
    /// \c FixedDisruptor so the scheduler and delivery engine can
    /// select the correct backend without RTTI.
    [[nodiscard]] virtual mailbox::MailboxKind mailbox_kind() const noexcept {
        return mailbox::MailboxKind::VariableMpsc;
    }

    /// \brief Create the fixed mailbox binding for this actor.
    ///
    /// Default returns an empty (invalid) binding.  Fixed-mailbox actors
    /// override to create their \c FixedActorMailboxCore and return a
    /// populated \c FixedMailboxHandle.
    [[nodiscard]] virtual mailbox::FixedMailboxHandle
    create_fixed_mailbox() noexcept {
        return {};
    }

    /// \brief Human-readable type name for metrics and CLI introspection.
    virtual std::string_view type_name() const {
        return type_name_;
    }
    /// \brief Set the type name. Called by \c ActorSystem during spawn.
    void set_type_name(std::string name) {
        type_name_ = std::move(name);
    }

    /// \brief Set the metrics ring buffer pointer for out-of-band events.
    ///
    /// Default no-op. Overridden by actors that emit metric events.
    /// \param[in] buf Opaque pointer to an \c MpscRingBuffer<MetricEvent>.
    virtual void set_metrics_ring_buffer(void* /*buf*/) {}
    /// \brief Set the logger pointer for structured logging.
    ///
    /// Default no-op. Overridden by actors that log from the hot path.
    /// \param[in] logger Opaque pointer to a \c Logger instance.
    virtual void set_logger(void* /*logger*/) noexcept {}

    /// \brief Return lightweight inspectable metadata for CLI introspection.
    ///
    /// Called from the actor's own thread via \c InspectStateRequest.
    virtual cli::ActorMeta to_metadata() const;

    /// \brief Return an opaque serialized state blob.
    ///
    /// Default returns empty. Stateful actors override this for hibernation.
    virtual std::vector<uint8_t> serialize_state() const {
        return {};
    }

    /// \brief Return a snapshot of the actor's mailbox.
    ///
    /// Default returns empty. Override in mailbox-owning actors for CLI
    /// inspection.
    virtual cli::MboxSnapshot mailbox_snapshot() const {
        return {};
    }

  protected:
    /// \brief Construct an actor with its identity and owning system.
    ///
    /// Called by subclasses and \c ActorSystem::spawn().
    /// \param[in] id Globally-unique actor identifier.
    /// \param[in] type Actor type tag.
    /// \param[in] sys Owning \c ActorSystem reference.
    AbstractActor(ActorId id, ActorType type, ActorSystem& sys);

    /// \brief Return the actor's \c ActorContext.
    ///
    /// Overridden by \c LocalActor. Returns \c nullptr for actors without
    /// a context (e.g., the system pseudo-actor).
    virtual ActorContext* actor_context() {
        return nullptr;
    }

  private:
    ActorId id_;
    ActorType type_;
    ActorSystem& system_;
    ActorAddress address_;
    std::string type_name_;
};

} // namespace hpactor
