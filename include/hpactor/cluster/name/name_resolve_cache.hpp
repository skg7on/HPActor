// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <chrono>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>

#include <hpactor/ref/actor_address.hpp>

namespace hpactor::cluster::name {

/// \brief TTL cache for name→ActorAddress resolution results.
///
/// Populated by NameResolver on successful remote resolution. Read-heavy
/// (shared_mutex with read-lock for get(), write-lock for put/evict).
///
/// \note Thread safety: All public methods are safe to call from any thread.
class NameResolveCache {
  public:
    /// \brief Look up a cached name→address mapping.
    /// \param[in] name Actor name.
    /// \return Cached ActorAddress, or std::nullopt if missing or expired.
    std::optional<ActorAddress> get(const std::string& name) const;

    /// \brief Insert or update a cached entry.
    /// \param[in] name Actor name.
    /// \param[in] addr Resolved address.
    /// \param[in] ttl Time-to-live for this entry.
    void put(const std::string& name, ActorAddress addr,
             std::chrono::seconds ttl);

    /// \brief Remove a specific name from the cache.
    /// \param[in] name Actor name to evict.
    void evict(const std::string& name);

    /// \brief Remove all entries pointing to a given endpoint.
    ///
    /// Called on node departure to purge cached resolution results for
    /// actors hosted on the departed node.
    ///
    /// \param[in] ep Endpoint whose cached entries should be purged.
    void evict_node(EndPoint ep);

    /// \brief Remove all expired entries.
    ///
    /// Call periodically to prevent unbounded growth from stale entries.
    /// Does not compact memory or shrink the underlying hashmap.
    void purge_expired();

  private:
    struct Entry {
        ActorAddress address;
        std::chrono::steady_clock::time_point expires_at;
    };
    std::unordered_map<std::string, Entry> cache_;
    mutable std::shared_mutex mutex_;
};

} // namespace hpactor::cluster::name
