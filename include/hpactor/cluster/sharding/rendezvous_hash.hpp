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

#include <hpactor/cluster/sharding/placement_strategy.hpp>

#include <string>

namespace hpactor::cluster::sharding {

/// \brief Rendezvous (Highest Random Weight) hash placement.
///
/// Deterministic, minimizes movement on node add/remove.
class RendezvousHash : public IPlacementStrategy {
  public:
    PlacementPlan plan(std::span<const ShardId> shards,
                       std::span<const std::string> alive_nodes,
                       std::span<const ShardEntry> current_assignments) override;
};

} // namespace hpactor::cluster::sharding
