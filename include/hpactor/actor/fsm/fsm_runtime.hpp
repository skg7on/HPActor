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

#include <chrono>
#include <functional>
#include <unordered_map>
#include <utility>
#include <vector>

#include <hpactor/actor/actor_context.hpp>
#include <hpactor/actor/fsm/fsm_directive.hpp>
#include <hpactor/msg/type_tag.hpp>
#include <hpactor/msg/typed_message.hpp>

namespace hpactor {

/// \brief Hash functor for \c std::pair used as handler map key.
struct PairHash {
    template <typename T1, typename T2>
    std::size_t operator()(const std::pair<T1, T2>& p) const noexcept {
        auto h1 = std::hash<T1>{}(p.first);
        auto h2 = std::hash<T2>{}(p.second);
        return h1 ^ (h2 << 1);
    }
};

/// \brief Shared runtime state for an FSM-backed behavior.
///
/// Owns the current state, data, per-state handler map, timeout
/// configuration, and transition hooks. Created by
/// \c BehaviorFsmBuilder and shared with the \c Behavior fallback
/// handler via \c shared_ptr.
///
/// \tparam S The state enum type (must be hashable with \c std::hash
///           and equality-comparable).
/// \tparam D The state data type (must be copy-constructible).
///
/// \note Thread safety: All methods must be called from the owning
///       actor's scheduler thread. No internal synchronization.
template <typename S, typename D> class FsmRuntime {
  public:
    using Directive = FsmDirective<S, D>;

    /// \brief Type-erased handler stored per (state, TypeTag).
    ///
    /// Receives the raw \c TypedMessage and a mutable reference to
    /// the current state data. Returns a directive controlling the
    /// next state transition.
    using ErasedHandler = std::function<Directive(TypedMessage&, D&)>;

    /// \brief Per-state timeout configuration.
    struct TimeoutConfig {
        /// \brief Idle duration before the timeout fires.
        ///        Zero means no timeout.
        std::chrono::milliseconds duration{0};
        /// \brief Target state to transition to on timeout.
        S target_state{};
        /// \brief Data to install on timeout transition.
        D target_data{};
    };

    /// \brief Current state of the FSM.
    S current_state{};
    /// \brief Current state data.
    D data{};
    /// \brief Per-state typed handlers keyed by (state, TypeTag).
    std::unordered_map<std::pair<S, TypeTag>, ErasedHandler, PairHash> handlers;
    /// \brief Per-state timeout configuration.
    std::unordered_map<S, TimeoutConfig> timeout_configs;
    /// \brief Transition hooks called on every \c GoTo.
    std::vector<std::function<void(S from, S to, D& data)>> transition_handlers;
    /// \brief Whether the FSM has been stopped.
    bool stopped{false};

    /// \brief Construct an FSM runtime with initial state and data.
    ///
    /// \param[in] initial_state The starting state.
    /// \param[in] initial_data The starting state data.
    FsmRuntime(S initial_state, D initial_data)
        : current_state(std::move(initial_state)), data(std::move(initial_data)) {}

    /// \brief Set the actor context for timer integration.
    ///
    /// Must be called before any messages are dispatched if per-state
    /// timeouts are configured. May be \c nullptr for timer-less use.
    /// \param[in] context The actor's \c ActorContext pointer, or
    ///                    \c nullptr.
    void set_context(ActorContext* context) noexcept {
        ctx_ = context;
    }

    /// \brief Set the reserved \c TypeTag used for FSM timeout messages.
    ///
    /// \param[in] tag The tag to use for timeout self-messages.
    void set_timeout_tag(TypeTag tag) noexcept {
        fsm_timeout_tag_ = tag;
    }

    /// \brief Dispatch an incoming message through the FSM.
    ///
    /// Looks up the handler for \c (current_state, msg.type_id()).
    /// If found, invokes it and processes the returned directive.
    /// If the message is the FSM timeout tag, triggers the timeout
    /// handler. Otherwise, the message is silently dropped.
    ///
    /// \param[in,out] msg The incoming typed message.
    void dispatch(TypedMessage& msg) {
        if (stopped)
            return;

        if (msg.type_id() == fsm_timeout_tag_) {
            handle_timeout();
            return;
        }

        auto it = handlers.find({current_state, msg.type_id()});
        if (it == handlers.end())
            return;

        Directive dir = it->second(msg, data);
        process_directive(dir);
    }

    /// \brief Process a directive returned by a handler.
    ///
    /// Handles \c Stay, \c GoTo, and \c Stop semantics including
    /// timer management and transition hook invocation.
    ///
    /// \param[in] dir The directive to process.
    void process_directive(const Directive& dir) {
        switch (dir.kind) {
            case Directive::Kind::Stay: {
                if (dir.update_data) {
                    data = dir.target_data;
                }
                cancel_active_timer();
                schedule_state_timeout();
                break;
            }
            case Directive::Kind::GoTo: {
                S old_state = current_state;
                cancel_active_timer();
                enter_state(dir.target_state, dir.target_data);
                fire_transition_hooks(old_state, dir.target_state);
                schedule_state_timeout();
                break;
            }
            case Directive::Kind::Stop: {
                cancel_active_timer();
                stopped = true;
                break;
            }
        }
    }

  private:
    void enter_state(S new_state, D new_data) {
        current_state = std::move(new_state);
        data = std::move(new_data);
    }

    void fire_transition_hooks(S from, S to) {
        for (auto& hook : transition_handlers) {
            if (hook)
                hook(from, to, data);
        }
    }

    void handle_timeout() {
        auto it = timeout_configs.find(current_state);
        if (it == timeout_configs.end())
            return;

        S old_state = current_state;
        enter_state(it->second.target_state, it->second.target_data);
        fire_transition_hooks(old_state, it->second.target_state);
        schedule_state_timeout();
    }

    void cancel_active_timer() {
        if (ctx_ && active_timer_.valid()) {
            ctx_->cancel_schedule(active_timer_);
            active_timer_ = AlarmHandle{};
        }
    }

    void schedule_state_timeout() {
        if (!ctx_)
            return;

        auto it = timeout_configs.find(current_state);
        if (it == timeout_configs.end())
            return;
        if (it->second.duration.count() == 0)
            return;

        StreamBuffer empty_payload;
        TypedMessage timeout_msg(fsm_timeout_tag_, empty_payload);
        active_timer_ =
            ctx_->schedule(it->second.duration, std::move(timeout_msg));
    }

    ActorContext* ctx_{nullptr};
    AlarmHandle active_timer_{};
    TypeTag fsm_timeout_tag_{TypeTag::Invalid};
};

} // namespace hpactor
