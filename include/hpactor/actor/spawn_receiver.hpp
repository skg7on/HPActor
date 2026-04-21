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
#include <hpactor/actor_type_registry.hpp>
#include <hpactor/net/frame.hpp>
#include <hpactor/spawn.hpp>

namespace hpactor {

// -----------------------------------------------------------------------------
// SpawnReceiver - system actor handling remote spawn requests
// -----------------------------------------------------------------------------
// Receives SpawnRequest messages, creates actors via ActorTypeRegistry,
// and sends SpawnResponse back to the caller via Transport.
class SpawnReceiver : public EventBasedActor {
public:
    SpawnReceiver(ActorSystem& sys, ActorTypeRegistry& registry, net::Transport* transport);

    Behavior make_behavior() override;

private:
    void handle_spawn_request(const SpawnRequest& req, const net::Frame& frame);

    ActorTypeRegistry& registry_;
    net::Transport* transport_;  // non-owning
};

} // namespace hpactor