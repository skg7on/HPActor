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

// ── Disruptor behavior
// ────────────────────────────────────────────────────────

/// \brief Handler table for a fixed-mailbox actor.
///
/// Stores one \c std::function per declared message type in a tuple.
/// Dispatch uses \c std::visit with a generic lambda that extracts
/// the matching handler by type.
///
/// \tparam Messages The closed set of fixed user-message types.
template <typename... Messages> struct DisruptorBehavior {
    std::tuple<std::function<void(const Messages&)>...> handlers;

    DisruptorBehavior() = default;

    DisruptorBehavior(std::function<void(const Messages&)>... hs) noexcept
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
/// Usage: \c on_disruptor<Increment>([this](const Increment& m) { ... })
template <typename T, typename Fn>
[[nodiscard]] std::function<void(const T&)> on_disruptor(Fn&& fn) {
    return std::function<void(const T&)>(std::forward<Fn>(fn));
}

// ── Disruptor mailbox actor
// ───────────────────────────────────────────────────

/// \brief Actor base class for fixed-message Disruptor mailbox actors.
///
/// Derives from \c EventBasedActor to retain actor state, lifecycle,
/// supervision, drain, system-message dispatch, logging, and metrics.
///
/// \tparam Capacity Power-of-two ring capacity.
/// \tparam Messages The closed set of fixed user-message types.
template <size_t Capacity, mailbox::DisruptorMessage... Messages>
class DisruptorMailboxActor
    : public EventBasedActor,
      public mailbox::DisruptorActorMailboxCore<Capacity, Messages...>::DispatchTarget {
  public:
    using core_type = mailbox::DisruptorActorMailboxCore<Capacity, Messages...>;
    using disruptor_actor_ref_type = DisruptorActorRef<Capacity, Messages...>;
    using disruptor_behavior_type = DisruptorBehavior<Messages...>;

    using EventBasedActor::EventBasedActor;

    /// \brief Return the mailbox backend kind.
    [[nodiscard]] mailbox::MailboxKind mailbox_kind() const noexcept override {
        return mailbox::MailboxKind::Disruptor;
    }

    // ── DispatchTarget implementation ──────────────────────────────────

    void on_system_message(TypedMessage&& msg) override {
        receive(msg);
    }

    void on_user_message(typename core_type::envelope_type& env) override {
        // Store reply context for ask/reply correlation.
        // When the handler calls context()->reply(), these fields enable
        // the reply to be routed back to the original sender and correlated
        // with the pending ask via AskManager.
        if (auto* ctx = context()) {
            ctx->set_current_sender(env.meta.sender);
            if (env.meta.ask_message_id != 0) {
                ctx->set_current_ask_message_id(env.meta.ask_message_id);
            }
        }
        disruptor_behavior_.dispatch(env.message);
    }

    void on_work_available() override {
        home_system().scheduler()->notify_ready(id(), 0, INT64_MAX);
    }

    // ── Lifecycle ──────────────────────────────────────────────────────

    /// \brief Drain all messages immediately (immediate stop).
    void drain_all_immediate() override {
        if (core_) {
            core_->begin_drain();
            while (!core_->ring().empty()) {
                auto lease = core_->ring().try_acquire();
            }
        }
        EventBasedActor::drain_all_immediate();
    }

    /// \brief Create the disruptor mailbox core and return a handle.
    mailbox::DisruptorMailboxHandle create_disruptor_mailbox() noexcept override {
        auto core = std::make_shared<core_type>(this->id(), this->address());
        core_ = core;
        disruptor_behavior_ = make_disruptor_behavior();
        core->set_target(this);
        return core->make_handle();
    }

    /// \brief Return a typed reference for sending to this actor.
    [[nodiscard]] disruptor_actor_ref_type disruptor_ref() const noexcept {
        return disruptor_actor_ref_type(this->address(), core_);
    }

  protected:
    /// \brief Build the handler table for this actor's message types.
    [[nodiscard]] virtual disruptor_behavior_type make_disruptor_behavior() = 0;

  private:
    std::shared_ptr<core_type> core_;
    disruptor_behavior_type disruptor_behavior_;
};

} // namespace hpactor
