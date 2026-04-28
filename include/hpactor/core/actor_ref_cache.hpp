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

#include <hpactor/ref/actor_ref.hpp>
#include <hpactor/types/types.hpp>

#include <optional>
#include <unordered_map>

namespace hpactor {

class ActorRefCache {
public:
    explicit ActorRefCache(size_t max_entries = kDefaultMaxEntries)
        : max_entries_(max_entries) {}

    std::optional<ActorRef> get(ActorId id) {
        auto it = cache_.find(id);
        if (it == cache_.end()) {
            return std::nullopt;
        }
        it->second.last_access_tick = ++tick_;
        return it->second.ref;
    }

    void put(ActorId id, ActorRef ref) {
        auto it = cache_.find(id);
        if (it != cache_.end()) {
            it->second.ref = std::move(ref);
            it->second.last_access_tick = ++tick_;
            return;
        }
        if (cache_.size() >= max_entries_) {
            evict_lru();
        }
        cache_.emplace(id, Entry{std::move(ref), ++tick_});
    }

private:
    static constexpr size_t kDefaultMaxEntries = 256;

    struct Entry {
        ActorRef ref;
        uint64_t last_access_tick = 0;
    };

    void evict_lru() {
        ActorId lru_id;
        uint64_t min_tick = UINT64_MAX;
        for (const auto& [id, entry] : cache_) {
            if (entry.last_access_tick < min_tick) {
                min_tick = entry.last_access_tick;
                lru_id = id;
            }
        }
        cache_.erase(lru_id);
    }

    std::unordered_map<ActorId, Entry> cache_;
    uint64_t tick_ = 0;
    size_t max_entries_;
};

} // namespace hpactor
