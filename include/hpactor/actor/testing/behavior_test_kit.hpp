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

#include <hpactor/actor/behavior.hpp>
#include <hpactor/msg/typed_message.hpp>

namespace hpactor::testing {

/// The kind of effect produced by running a message through a Behavior.
enum class EffectKind {
    NoEffect, ///< No handler processed the message (empty behavior).
    Handled,  ///< The behavior processed the message.
};

/// Result of BehaviorTestKit::run().
struct Effect {
    EffectKind kind{EffectKind::NoEffect};
};

/// Synchronous Behavior testing harness — no ActorSystem, no scheduler,
/// no threads. Send a TypedMessage and verify dispatch.
///
/// Usage:
///   Behavior b = Behavior::make().on<MyMsg>([](const MyMsg&) { ... });
///   BehaviorTestKit kit(b);
///   auto effect = kit.run(TypedMessage(MyMsg::kTypeTag, my_msg));
///   EXPECT_EQ(effect.kind, EffectKind::Handled);
class BehaviorTestKit {
  public:
    explicit BehaviorTestKit(Behavior behavior)
        : current_behavior_(std::move(behavior)) {}

    /// Run a TypedMessage through the behavior.
    Effect run(TypedMessage& msg) {
        if (!current_behavior_)
            return Effect{EffectKind::NoEffect};
        current_behavior_(msg);
        return Effect{EffectKind::Handled};
    }

    /// The current behavior.
    const Behavior& current_behavior() const {
        return current_behavior_;
    }

    /// Simulate a become() transition.
    void become(Behavior b) {
        current_behavior_ = std::move(b);
    }

  private:
    Behavior current_behavior_;
};

} // namespace hpactor::testing
