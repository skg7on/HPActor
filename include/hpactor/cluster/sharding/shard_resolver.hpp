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

#include <hpactor/cluster/sharding/shard_types.hpp>

#include <cstdint>

namespace hpactor::cluster::sharding {

/// \brief Deterministic mapping from LogicalActorId to ShardId.
///
/// Uses std::hash modulo total_shards. The mapping is stable for a given
/// total_shards value — changing total_shards redistributes all actors.
class ShardResolver {
  public:
    /// \brief Map a logical actor ID to a shard ID.
    ///
    /// \param[in] id The logical actor identity.
    /// \param[in] total_shards The total number of shards.
    /// \return The shard ID in [0, total_shards).
    static ShardId resolve(const LogicalActorId& id, uint32_t total_shards);
};

} // namespace hpactor::cluster::sharding
