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

#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/mailbox/disruptor_actor_mailbox.hpp>
#include <hpactor/mailbox/disruptor_mailbox_interface.hpp>
#include <hpactor/mailbox/mailbox_kind.hpp>
#include <hpactor/ref/disruptor_actor_ref.hpp>

#include <functional>
#include <memory>
#include <tuple>
#include <variant>

namespace hpactor {

// ── Fixed behavior ────────────────────────────────────────────────────────

/// \brief Handler table for a fixed-mailbox actor.
///
/// Stores one \c std::function per declared message type in a tuple.
/// Dispatch uses \c std::visit with a generic lambda that extracts
/// the matching handler by type.
///
/// \tparam Messages The closed set of fixed user-message types.
template <typename... Messages> struct FixedBehavior {
    std::tuple<std::function<void(const Messages&)>...> handlers;

    FixedBehavior() = default;

    FixedBehavior(std::function<void(const Messages&)>... hs) noexcept
        : handlers(std::move(hs)...) {}

    /// \brief Dispatch the active variant alternative to its handler.
    void dispatch(const std::variant<Messages...>& msg) const {
        std::visit(
            [this]<typename T>(const T& value) {
                auto& handler = std::get<std::function<void(const T&)>>(handlers);
                if (handler) {
                    handler(value);
                }
            },
            msg);
    }
};

/// \brief Create a handler wrapper for a specific fixed-message type.
///
/// Usage: \c on_fixed<Increment>([this](const Increment& m) { ... })
template <typename T, typename Fn>
[[nodiscard]] std::function<void(const T&)> on_fixed(Fn&& fn) {
    return std::function<void(const T&)>(std::forward<Fn>(fn));
}

// ── Fixed mailbox actor ───────────────────────────────────────────────────

/// \brief Actor base class for fixed-message Disruptor mailbox actors.
///
/// Derives from \c EventBasedActor to retain actor state, lifecycle,
/// supervision, drain, system-message dispatch, logging, and metrics.
///
/// \tparam Capacity Power-of-two ring capacity.
/// \tparam Messages The closed set of fixed user-message types.
template <size_t Capacity, mailbox::FixedMailboxMessage... Messages>
class FixedMailboxActor : public EventBasedActor {
  public:
    using core_type = mailbox::FixedActorMailboxCore<Capacity, Messages...>;
    using fixed_actor_ref_type = FixedActorRef<Capacity, Messages...>;
    using fixed_behavior_type = FixedBehavior<Messages...>;

    using EventBasedActor::EventBasedActor;

    /// \brief Return the mailbox backend kind.
    [[nodiscard]] mailbox::MailboxKind mailbox_kind() const noexcept override {
        return mailbox::MailboxKind::FixedDisruptor;
    }

    /// \brief Drain all messages immediately (immediate stop).
    void drain_all_immediate() override {
        if (core_) {
            core_->begin_drain();
            // Release all user-ring slots without invoking handlers.
            while (!core_->ring().empty()) {
                auto lease = core_->ring().try_acquire();
                // Lease releases on scope exit without dispatch.
            }
        }
        EventBasedActor::drain_all_immediate();
    }

    /// \brief Create the fixed mailbox core and return a populated binding.
    mailbox::FixedMailboxHandle create_fixed_mailbox() noexcept override {
        auto core = std::make_shared<core_type>(this->id(), this->address());
        core_ = core;
        fixed_behavior_ = make_fixed_behavior();
        core->set_dispatch_callbacks(
            this,
            /* system_fn */
            +[](void* ctx, TypedMessage&& msg) noexcept {
                auto* self = static_cast<FixedMailboxActor*>(ctx);
                self->receive(msg);
            },
            /* user_fn */
            +[](void* ctx, void* envelope) noexcept {
                auto* self = static_cast<FixedMailboxActor*>(ctx);
                auto* env =
                    static_cast<typename core_type::envelope_type*>(envelope);
                self->fixed_behavior_.dispatch(env->message);
            });
        core->set_signal_callback(
            +[](void* ctx) noexcept {
                auto* self = static_cast<FixedMailboxActor*>(ctx);
                self->home_system().get_scheduler()->notify_ready(self->id(), 0,
                                                                  INT64_MAX);
            },
            this);
        return core->make_handle();
    }

    /// \brief Return a typed reference for sending to this actor.
    [[nodiscard]] fixed_actor_ref_type fixed_ref() const noexcept {
        return fixed_actor_ref_type(this->address(), core_);
    }

  protected:
    /// \brief Build the handler table for this actor's message types.
    [[nodiscard]] virtual fixed_behavior_type make_fixed_behavior() = 0;

  private:
    std::shared_ptr<core_type> core_;
    fixed_behavior_type fixed_behavior_;
};

} // namespace hpactor
