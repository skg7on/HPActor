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
#include <hpactor/actor/receptionist/receptionist_messages.hpp>

#include <unordered_map>
#include <unordered_set>

namespace hpactor::receptionist {

/// System actor that provides publish/subscribe actor lookup by
/// ServiceKey. Actors register themselves under a key; other actors
/// subscribe to receive Listing notifications when the key's
/// membership set changes.
class Receptionist : public EventBasedActor {
  public:
    Receptionist(ActorContext* ctx, ActorSystem& sys);

    Behavior make_behavior() override;

    /// Thread-safe queries for testing and CLI introspection.
    size_t registration_count() const;
    size_t subscription_count() const;

    /// Direct handler access for testing with injected messages.
    void handle_register(const Register& msg);
    void handle_subscribe(const Subscribe& msg);
    void handle_unregister(const Unregister& msg);
    void handle_unsubscribe(const Unsubscribe& msg);

    /// Build a Listing for a key (for testing).
    Listing build_listing(const ServiceKey& key);

  private:
    /// Broadcast a Listing to all subscribers of `key`.
    void broadcast_listing(const ServiceKey& key);

    std::unordered_map<ServiceKey, std::unordered_set<ActorId>> registry_;
    std::unordered_map<ServiceKey, std::unordered_set<ActorId>> subscribers_;
};

} // namespace hpactor::receptionist
