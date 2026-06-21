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

#include <cstdint>
#include <string>

namespace hpactor::cluster::sharding {

/// \brief Identifies a shard — a partition of the logical actor keyspace.
using ShardId = uint32_t;

/// \brief A cached shard-to-owner mapping entry with epoch for invalidation.
struct ShardEntry {
    ShardId shard_id = 0;
    std::string owner_node;
    uint64_t epoch = 0;
};

/// \brief Stable application-level identity for an actor.
/// Maps deterministically to a shard via the ShardResolver.
struct LogicalActorId {
    std::string persistence_id; ///< e.g. "tenant-42/session-9"
};

} // namespace hpactor::cluster::sharding
