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

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <utility>

#include <hpactor/actor/behavior.hpp>
#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/actor/fsm/fsm_directive.hpp>
#include <hpactor/actor/fsm/fsm_runtime.hpp>
#include <hpactor/msg/proto_type_registry.hpp>

namespace hpactor {

// Forward declarations.
template <typename S, typename D> class BehaviorFsmBuilder;

/// \brief Fluent builder for configuring handlers in a single FSM state.
///
/// Returned by \c BehaviorFsmBuilder::in_state(). Supports chaining
/// \c on<T>() for typed protobuf message handlers, \c on_raw() for
/// raw \c TypeTag handlers, and \c on_timeout() for per-state idle
/// timeouts. Call \c in_state() again to move to the next state, or
/// \c build() to finalize.
///
/// \tparam S The state enum type.
/// \tparam D The state data type.
template <typename S, typename D> class StateBuilder {
  public:
    using Directive = FsmDirective<S, D>;

    /// \brief Construct a StateBuilder for a specific state.
    StateBuilder(BehaviorFsmBuilder<S, D>* parent, S state)
        : parent_(parent), state_(std::move(state)) {}

    /// \brief Register a typed handler for a protobuf message type.
    ///
    /// The handler receives a const reference to the deserialized
    /// message and a mutable reference to the current state data.
    /// Returns a directive controlling the next transition.
    ///
    /// \tparam T Protobuf message type (must have
    ///           \c MessageTraits<T> specialization).
    template <typename T>
    StateBuilder& on(std::function<Directive(const T&, D&)> handler) {
        TypeTag tag = MessageTraits<T>::tag();
        auto& runtime = parent_->runtime();
        runtime->handlers[{state_, tag}] =
            [h = std::move(handler)](TypedMessage& msg, D& data) -> Directive {
            auto parsed = msg.as<T>();
            if (parsed)
                return h(*parsed, data);
            return Directive::stay();
        };
        return *this;
    }

    /// \brief Register a raw handler for a specific \c TypeTag.
    StateBuilder&
    on_raw(TypeTag tag, std::function<Directive(TypedMessage&, D&)> handler) {
        parent_->runtime()->handlers[{state_, tag}] = std::move(handler);
        return *this;
    }

    /// \brief Configure an idle timeout for this state.
    StateBuilder&
    on_timeout(std::chrono::milliseconds duration, S target_state, D target_data) {
        typename FsmRuntime<S, D>::TimeoutConfig cfg;
        cfg.duration = duration;
        cfg.target_state = std::move(target_state);
        cfg.target_data = std::move(target_data);
        parent_->runtime()->timeout_configs[state_] = std::move(cfg);
        return *this;
    }

    /// \brief Begin configuring handlers for another state.
    StateBuilder in_state(S next_state) {
        return StateBuilder(parent_, std::move(next_state));
    }

    /// \brief Register a transition handler (delegates to parent).
    StateBuilder&
    on_transition(std::function<void(S from, S to, D& data)> handler) {
        parent_->on_transition(std::move(handler));
        return *this;
    }

    /// \brief Finalize the FSM and produce a \c Behavior.
    Behavior build(EventBasedActor* actor) {
        return parent_->build(actor);
    }

  private:
    BehaviorFsmBuilder<S, D>* parent_;
    S state_;
};

/// \brief Fluent builder for constructing an FSM-backed \c Behavior.
///
/// Provides a declarative API for defining states, per-state typed
/// message handlers, per-state idle timeouts, and state-transition
/// hooks. The builder produces a standard \c Behavior that can be
/// returned from \c EventBasedActor::make_behavior().
///
/// Usage:
/// \code{.cpp}
/// enum class State { A, B };
/// struct Data { int count = 0; };
///
/// Behavior make_behavior() override {
///     return BehaviorFsmBuilder<State, Data>
///         ::start(State::A, Data{})
///         .in_state(State::A)
///             .on<GoToB>([this](const GoToB&, Data& d) {
///                 return FsmDirective<State,Data>::go_to(State::B, d);
///             })
///         .in_state(State::B)
///             .on<GoToA>(...)
///             .on_timeout(std::chrono::seconds(30), State::A, Data{})
///         .on_transition([](State from, State to, Data& d) {
///             // log, metrics, audit
///         })
///         .build(this);
/// }
/// \endcode
template <typename S, typename D> class BehaviorFsmBuilder {
  public:
    using Directive = FsmDirective<S, D>;

    /// \brief Create a builder starting in \p initial_state.
    static BehaviorFsmBuilder start(S initial_state, D initial_data) {
        BehaviorFsmBuilder builder;
        builder.runtime_ = std::make_shared<FsmRuntime<S, D>>(
            std::move(initial_state), std::move(initial_data));
        return builder;
    }

    /// \brief Begin configuring handlers for a state.
    StateBuilder<S, D> in_state(S state) {
        return StateBuilder<S, D>(this, std::move(state));
    }

    /// \brief Register a transition handler called on every \c GoTo.
    BehaviorFsmBuilder&
    on_transition(std::function<void(S from, S to, D& data)> handler) {
        runtime_->transition_handlers.push_back(std::move(handler));
        return *this;
    }

    /// \brief Finalize the FSM and produce a \c Behavior.
    Behavior build(EventBasedActor* actor) {
        if (actor)
            runtime_->set_context(actor->context());
        runtime_->set_timeout_tag(allocate_timeout_tag());

        auto rt = runtime_;
        return Behavior([rt](TypedMessage& msg) { rt->dispatch(msg); });
    }

    /// \brief Access the underlying runtime (for testing/introspection).
    std::shared_ptr<FsmRuntime<S, D>> runtime() const {
        return runtime_;
    }

  private:
    static TypeTag allocate_timeout_tag() {
        // Start from a high base in the application range to avoid
        // collisions with protobuf TypeTag values.
        static std::atomic<uint64_t> counter{0x7E000000ULL};
        return static_cast<TypeTag>(counter.fetch_add(1, std::memory_order_relaxed));
    }

    std::shared_ptr<FsmRuntime<S, D>> runtime_;
};

} // namespace hpactor
