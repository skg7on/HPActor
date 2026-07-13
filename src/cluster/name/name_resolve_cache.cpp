// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#include <hpactor/cluster/name/name_resolve_cache.hpp>

#include <mutex>
#include <shared_mutex>

namespace hpactor::cluster::name {

std::optional<ActorAddress>
NameResolveCache::get(const std::string& name) const {
    std::shared_lock lock(mutex_);
    auto it = cache_.find(name);
    if (it == cache_.end())
        return std::nullopt;
    if (it->second.expires_at <= std::chrono::steady_clock::now())
        return std::nullopt;
    return it->second.address;
}

void NameResolveCache::put(const std::string& name, ActorAddress addr,
                            std::chrono::seconds ttl) {
    std::unique_lock lock(mutex_);
    cache_[name] = {addr, std::chrono::steady_clock::now() + ttl};
}

void NameResolveCache::evict(const std::string& name) {
    std::unique_lock lock(mutex_);
    cache_.erase(name);
}

void NameResolveCache::evict_node(EndPoint ep) {
    std::unique_lock lock(mutex_);
    for (auto it = cache_.begin(); it != cache_.end();) {
        if (it->second.address.endpoint == ep) {
            it = cache_.erase(it);
        } else {
            ++it;
        }
    }
}

void NameResolveCache::purge_expired() {
    std::unique_lock lock(mutex_);
    auto now = std::chrono::steady_clock::now();
    for (auto it = cache_.begin(); it != cache_.end();) {
        if (it->second.expires_at <= now) {
            it = cache_.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace hpactor::cluster::name
