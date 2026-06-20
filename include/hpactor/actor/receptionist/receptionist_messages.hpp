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

#include <hpactor/actor/receptionist/service_key.hpp>
#include <hpactor/types/types.hpp>

#include <string>
#include <vector>

namespace hpactor::receptionist {

/// Message: register an actor under a ServiceKey.
struct Register {
    ServiceKey key;
    ActorId actor_id;
};

/// Message: subscribe to changes for a ServiceKey.
struct Subscribe {
    ServiceKey key;
    ActorId subscriber_id;
};

/// Message: unregister an actor from a ServiceKey.
struct Unregister {
    ServiceKey key;
    ActorId actor_id;
};

/// Message: unsubscribe from a ServiceKey.
struct Unsubscribe {
    ServiceKey key;
    ActorId subscriber_id;
};

/// Message: current listing sent to subscribers when the key's
/// membership set changes.
struct Listing {
    ServiceKey key;
    std::vector<ActorId> actor_ids;
};

} // namespace hpactor::receptionist
