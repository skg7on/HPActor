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

#include <hpactor/actor/behavior.hpp>

#include <memory>
#include <utility>

namespace hpactor {

// ═════════════════════════════════════════════════════════════════
// ComposeState::invoke — combinator dispatch
// ═════════════════════════════════════════════════════════════════

void Behavior::ComposeState::invoke(TypedMessage& msg, const Behavior& /*self*/) {
    switch (type) {
        case Type::Intercept: {
            // Build a next_fn that delegates to the inner behavior's
            // full dispatch (typed handlers + fallback).
            // Capture a copy of the shared_ptr to keep inner alive.
            auto inner_ptr = inner;
            auto next = [inner_ptr](TypedMessage& m) { (*inner_ptr)(m); };
            interceptor(msg, next);
            break;
        }
        case Type::Compose:
            if (inner)
                (*inner)(msg);
            if (second)
                (*second)(msg);
            break;
        case Type::OnSignal:
            if (msg.type_id() == signal_tag) {
                if (signal_handler)
                    signal_handler(msg);
            } else {
                if (inner)
                    (*inner)(msg);
            }
            break;
        case Type::Setup:
            if (!initialized) {
                inner = std::make_shared<Behavior>(factory());
                initialized = true;
            }
            if (inner)
                (*inner)(msg);
            break;
        case Type::MessageAdapter:
            if (msg.type_id() == adapter_from_tag && adapter_fn && inner) {
                auto translated = adapter_fn(msg);
                translated.set_trace_context(msg.trace_context());
                (*inner)(translated);
            } else if (inner) {
                (*inner)(msg);
            }
            break;
    }
}

// ═════════════════════════════════════════════════════════════════
// Behavior::intercept — middleware combinator
// ═════════════════════════════════════════════════════════════════

Behavior
Behavior::intercept(Behavior inner,
                    std::function<void(TypedMessage&, next_fn)> interceptor) {
    if (!inner && !interceptor)
        return Behavior::empty();

    auto state = std::make_shared<ComposeState>();
    state->type = ComposeState::Type::Intercept;
    state->inner = std::make_shared<Behavior>(std::move(inner));
    state->interceptor = std::move(interceptor);

    Behavior result;
    result.compose_ = std::move(state);
    return result;
}

// ═════════════════════════════════════════════════════════════════
// Behavior::compose — chain combinator
// ═════════════════════════════════════════════════════════════════

Behavior Behavior::compose(Behavior first, Behavior second) {
    if (!first && !second)
        return Behavior::empty();
    if (!first)
        return second;
    if (!second)
        return first;

    auto state = std::make_shared<ComposeState>();
    state->type = ComposeState::Type::Compose;
    state->inner = std::make_shared<Behavior>(std::move(first));
    state->second = std::make_shared<Behavior>(std::move(second));

    Behavior result;
    result.compose_ = std::move(state);
    return result;
}

// ═════════════════════════════════════════════════════════════════
// Behavior::on_signal — signal interception combinator
// ═════════════════════════════════════════════════════════════════

Behavior Behavior::on_signal(TypeTag tag, handler_type handler, Behavior inner) {
    if (!handler && !inner)
        return Behavior::empty();
    if (!handler)
        return inner;

    auto state = std::make_shared<ComposeState>();
    state->type = ComposeState::Type::OnSignal;
    state->signal_tag = tag;
    state->signal_handler = std::move(handler);
    state->inner = std::make_shared<Behavior>(std::move(inner));

    Behavior result;
    result.compose_ = std::move(state);
    return result;
}

// ═════════════════════════════════════════════════════════════════
// Behavior::setup — deferred-init combinator
// ═════════════════════════════════════════════════════════════════

Behavior Behavior::setup(std::function<Behavior()> factory) {
    if (!factory)
        return Behavior::empty();

    auto state = std::make_shared<ComposeState>();
    state->type = ComposeState::Type::Setup;
    state->factory = std::move(factory);
    state->initialized = false;

    Behavior result;
    result.compose_ = std::move(state);
    return result;
}

} // namespace hpactor
