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

#include <hpactor/cluster/sharding/shard_resolver.hpp>

#include <functional>

namespace hpactor::cluster::sharding {

ShardId ShardResolver::resolve(const LogicalActorId& id, uint32_t total_shards) {
    if (total_shards == 0)
        return 0;
    size_t hash = std::hash<std::string>{}(id.persistence_id);
    return static_cast<ShardId>(hash % total_shards);
}

} // namespace hpactor::cluster::sharding
