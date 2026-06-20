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

#include <hpactor/actor/blocking_actor.hpp>
#include <hpactor/msg/proto_type_registry.hpp>

namespace hpactor {

// Forward declaration
class ActorSystem;

// -----------------------------------------------------------------------------
// ScopedActor - blocking actor for non-actor contexts (e.g., main function).
//
// Manages its own lifecycle: constructs with an ActorSystem, registers
// itself, and cleans up on destruction.  Does not go through the normal
// spawn path.
// -----------------------------------------------------------------------------
class ScopedActor : public BlockingActor {
  public:
    using AbstractActor::receive;

    explicit ScopedActor(ActorSystem& sys);
    ~ScopedActor() override;

    // Block until a message of type T arrives, then return it.
    template <typename T> T receive() {
        T result{};
        bool received = false;

        BlockingActor::receive([&](const TypedMessage& msg) {
            if (msg.type_id() == MessageTraits<T>::tag()) {
                const auto& buf = msg.payload();
                if (result.ParseFromArray(buf.data(), static_cast<int>(buf.size()))) {
                    received = true;
                }
            }
        });
        (void)received;

        return result;
    }
};

} // namespace hpactor
