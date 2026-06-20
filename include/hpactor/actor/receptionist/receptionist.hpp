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

#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace hpactor::receptionist {

/// System actor that provides publish/subscribe actor lookup by
/// ServiceKey. All methods are thread-safe (protected by internal
/// mutex). Uses direct function calls rather than TypedMessage dispatch.
class Receptionist : public EventBasedActor {
  public:
    Receptionist(ActorContext* ctx, ActorSystem& sys);

    Behavior make_behavior() override;

    /// Register an actor address under a ServiceKey.
    void register_actor(const ServiceKey& key, const ActorAddress& addr);

    /// Unregister an actor address from a ServiceKey.
    void unregister_actor(const ServiceKey& key, const ActorAddress& addr);

    /// Subscribe to membership changes for a ServiceKey.
    void add_subscriber(const ServiceKey& key, const ActorAddress& subscriber);

    /// Unsubscribe from a ServiceKey.
    void remove_subscriber(const ServiceKey& key, const ActorAddress& subscriber);

    /// Get the current listing for a key.
    Listing get_listing(const ServiceKey& key) const;

    /// Query counts (for testing and CLI introspection).
    size_t registration_count() const;
    size_t subscriber_count() const;

  private:
    mutable std::mutex mutex_;
    using AddrSet = std::unordered_set<ActorAddress>;
    std::unordered_map<ServiceKey, AddrSet> registry_;
    std::unordered_map<ServiceKey, AddrSet> subscribers_;
};

} // namespace hpactor::receptionist
