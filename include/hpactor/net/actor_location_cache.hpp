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

#include <hpactor/ref/actor_address.hpp>
#include <hpactor/types/types.hpp>

#include <chrono>
#include <optional>
#include <shared_mutex>
#include <unordered_map>

namespace hpactor::net {

/// \brief TTL cache for resolving \c ActorId to \c EndPoint.
///
/// Caches actor-to-endpoint mappings with configurable TTL to reduce
/// discovery lookups. Entries are automatically purged on expiry.
///
/// \note Thread safety: All public methods are safe to call from any
///       thread. Internal synchronization uses \c std::shared_mutex
///       (read-lock for \c get(), write-lock for \c put()/evict()).
class ActorLocationCache {
  public:
    /// \brief Look up the endpoint for an actor.
    ///
    /// \param[in] id Actor identifier.
    /// \return The cached endpoint, or \c std::nullopt if not found or
    ///         expired.
    std::optional<EndPoint> get(ActorId id) const;

    /// \brief Insert or update a cached entry.
    ///
    /// \param[in] id Actor identifier.
    /// \param[in] ep Target endpoint.
    /// \param[in] ttl Time-to-live for this entry (default 30 seconds).
    void put(ActorId id, EndPoint ep,
             std::chrono::seconds ttl = std::chrono::seconds(30));

    /// \brief Remove a specific actor from the cache.
    ///
    /// \param[in] id Actor identifier to evict.
    void evict(ActorId id);

    /// \brief Remove all entries pointing to a given endpoint.
    ///
    /// Used when a node leaves the cluster.
    /// \param[in] ep Node endpoint whose entries should be purged.
    void evict_node(EndPoint ep);

    /// \brief Remove all expired entries.
    ///
    /// Call periodically or on cache pressure. Returns immediately; does
    /// not compact memory.
    void purge_expired();

  private:
    /// \brief Single cache entry with TTL.
    struct Entry {
        EndPoint endpoint;
        std::chrono::steady_clock::time_point expires_at;
    };
    std::unordered_map<ActorId, Entry> cache_;
    mutable std::shared_mutex mutex_;
};

} // namespace hpactor::net
