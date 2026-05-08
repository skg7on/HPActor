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

#include <hpactor/types/types.hpp>
#include <hpactor/ref/actor_address.hpp>

#include <chrono>
#include <optional>
#include <shared_mutex>
#include <unordered_map>

namespace hpactor::net {

class ActorLocationCache {
public:
    std::optional<EndPoint> get(ActorId id) const;
    void put(ActorId id, EndPoint ep,
             std::chrono::seconds ttl = std::chrono::seconds(30));
    void evict(ActorId id);
    void evict_node(EndPoint ep);
    void purge_expired();

private:
    struct Entry {
        EndPoint endpoint;
        std::chrono::steady_clock::time_point expires_at;
    };
    std::unordered_map<ActorId, Entry> cache_;
    mutable std::shared_mutex mutex_;
};

} // namespace hpactor::net
