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

#include <hpactor/actor/spawn_receiver.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/net/frame.hpp>
#include <hpactor/types/serialization.hpp>

namespace hpactor {

SpawnReceiver::SpawnReceiver(ActorSystem& sys,
                            ActorTypeRegistry& registry,
                            net::Transport* transport)
    : EventBasedActor(nullptr, sys), registry_(registry), transport_(transport) {}

Behavior SpawnReceiver::make_behavior() {
    return Behavior{[this](MessageVariant&& msg) {
        std::visit([this](auto&& m) {
            using T = std::decay_t<decltype(m)>;
            if constexpr (std::is_same_v<T, SpawnRequest>) {
                handle_spawn_request(m);
            }
        }, std::move(msg));
    }};
}

void SpawnReceiver::handle_spawn_request(const SpawnRequest& req) {
    SpawnResponse response;

    auto result = registry_.spawn(system(), req.actor_type_name);
    if (result.has_value()) {
        response.actor_addr = result.value();
        response.error_code = spawn_errors::success;
    } else {
        response.error_code = result.error().code();
    }

    // TODO: Send response back via transport using Frame.message_id to route
    // This requires transport to support reply routing
    (void)response;
    (void)transport_;
}

} // namespace hpactor