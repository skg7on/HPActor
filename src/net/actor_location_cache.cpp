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

#include <hpactor/net/actor_location_cache.hpp>

namespace hpactor::net {

std::optional<EndPoint> ActorLocationCache::get(ActorId id) const {
    std::shared_lock lock(mutex_);
    auto it = cache_.find(id);
    if (it == cache_.end()) return std::nullopt;
    if (it->second.expires_at <= std::chrono::steady_clock::now())
        return std::nullopt;  // expired, deferred eviction via purge_expired()
    return it->second.endpoint;
}

void ActorLocationCache::put(ActorId id, EndPoint ep, std::chrono::seconds ttl) {
    std::unique_lock lock(mutex_);
    cache_[id] = {ep, std::chrono::steady_clock::now() + ttl};
}

void ActorLocationCache::evict(ActorId id) {
    std::unique_lock lock(mutex_);
    cache_.erase(id);
}

void ActorLocationCache::evict_node(EndPoint ep) {
    std::unique_lock lock(mutex_);
    for (auto it = cache_.begin(); it != cache_.end();) {
        if (it->second.endpoint == ep) it = cache_.erase(it);
        else ++it;
    }
}

void ActorLocationCache::purge_expired() {
    std::unique_lock lock(mutex_);
    auto now = std::chrono::steady_clock::now();
    for (auto it = cache_.begin(); it != cache_.end();) {
        if (it->second.expires_at <= now) it = cache_.erase(it);
        else ++it;
    }
}

} // namespace hpactor::net
