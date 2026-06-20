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
#include <hpactor/msg/type_tag.hpp>
#include <hpactor/ref/actor_address.hpp>

#include <vector>

namespace hpactor::receptionist {

/// Message: register an actor under a ServiceKey.
struct Register {
    static constexpr TypeTag kTypeTag = TypeTag::ReceptionistRegisterTag;
    ServiceKey key;
    ActorAddress address;
};

/// Message: subscribe to changes for a ServiceKey.
struct Subscribe {
    static constexpr TypeTag kTypeTag = TypeTag::ReceptionistSubscribeTag;
    ServiceKey key;
    ActorAddress subscriber;
};

/// Message: unregister an actor from a ServiceKey.
struct Unregister {
    static constexpr TypeTag kTypeTag = TypeTag::ReceptionistUnregisterTag;
    ServiceKey key;
    ActorAddress address;
};

/// Message: unsubscribe from a ServiceKey.
struct Unsubscribe {
    static constexpr TypeTag kTypeTag = TypeTag::ReceptionistUnsubscribeTag;
    ServiceKey key;
    ActorAddress subscriber;
};

/// Message: current listing sent to subscribers when the key's
/// membership set changes.
struct Listing {
    static constexpr TypeTag kTypeTag = TypeTag::ReceptionistListingTag;
    ServiceKey key;
    std::vector<ActorAddress> addresses;
};

} // namespace hpactor::receptionist
