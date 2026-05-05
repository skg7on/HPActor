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

#include <hpactor/actor/typed_message.hpp>
#include <hpactor/ref/actor_address.hpp>
#include <hpactor/types/types.hpp>
#include <hpactor/sched/dispatch_policy.hpp>

#include <memory>
#include <string>
#include <string_view>

namespace hpactor {

// Forward declarations
class ActorContext;
class ActorSystem;
namespace net {
enum class OpType : uint32_t;
} // namespace net
namespace sched {
class IScheduler;
} // namespace sched
namespace mailbox {
template <typename T> class MPSCActorMailbox;
} // namespace mailbox

// -----------------------------------------------------------------------------
// AbstractActor - base class for all actors
// -----------------------------------------------------------------------------
class AbstractActor : public std::enable_shared_from_this<AbstractActor> {
  public:
    virtual ~AbstractActor() = default;

    ActorId id() const {
        return id_;
    }
    const ActorId* id_ptr() const { return &id_; }
    ActorType type() const {
        return type_;
    }
    ActorAddress address() const {
        return address_;
    }
    ActorSystem& system() {
        return system_;
    }
    const ActorSystem& system() const {
        return system_;
    }

    // Set actor address (called by ActorSystem during spawn)
    void set_address(ActorAddress addr) {
        address_ = addr;
        id_ = addr.id;
        type_ = addr.type;
    }

    // Set scheduler and mailbox (called by ActorSystem during spawn)
    virtual void set_scheduler(sched::IScheduler* scheduler);
    virtual void
    set_mailbox(mailbox::MPSCActorMailbox<TypedMessage>* mailbox);

    // Linking - death sharing
    void link_to(const ActorAddr& other);
    void unlink_from(const ActorAddr& other);

    // Monitoring - receive down messages
    void monitor(const ActorAddr& target);
    void demonitor(const ActorAddr& target);

    // Receive message (called by scheduler)
    virtual void receive(TypedMessage& msg) = 0;

    // Type query for safe downcasting without RTTI
    virtual bool is_event_based_actor() const {
        return false;
    }

    // Dispatch policy — tells the scheduler how to execute this actor.
    // Default: Cooperative (M:N work-stealing pool).
    virtual sched::DispatchPolicy dispatch_policy() const {
        return sched::DispatchPolicy::Cooperative;
    }
    virtual sched::DispatchHints dispatch_hints() const {
        return {};
    }

    virtual std::string_view type_name() const { return type_name_; }
    void set_type_name(std::string name) { type_name_ = std::move(name); }

    virtual void set_metrics_ring_buffer(void* /*buf*/) {}

  protected:
    AbstractActor(ActorId id, ActorType type, ActorSystem& sys);

    // Overridden by LocalActor to return the ActorContext.
    // Returns nullptr for actors without a context (e.g., system actor).
    virtual ActorContext* actor_context() { return nullptr; }

  private:
    ActorId id_;
    ActorType type_;
    ActorSystem& system_;
    ActorAddress address_;
    std::string type_name_;
};

} // namespace hpactor
