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
