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

#include <gtest/gtest.h>

#include <hpactor/cluster/sharding/placement_strategy.hpp>
#include <hpactor/cluster/sharding/rendezvous_hash.hpp>
#include <hpactor/cluster/sharding/static_placement.hpp>

#include <vector>

namespace hpactor::cluster::sharding {

// Helper to build node lists
std::vector<std::string> make_nodes(std::initializer_list<std::string> nodes) {
    return std::vector<std::string>{nodes};
}

// ── StaticPlacement tests ──

TEST(StaticPlacementTest, EmptyConfigReturnsNoOwner) {
    StaticPlacement placement;
    const auto shards = std::vector<ShardId>{1, 2, 3};
    auto nodes = make_nodes({"node-a", "node-b"});
    auto plan = placement.plan(shards, nodes, {});
    EXPECT_TRUE(plan.assignments.empty());
}

TEST(StaticPlacementTest, ConfiguredMappingIsReturned) {
    StaticPlacement placement;
    placement.set_mapping(1, "node-a");
    placement.set_mapping(2, "node-b");

    const auto shards = std::vector<ShardId>{1, 2};
    auto nodes = make_nodes({"node-a", "node-b"});
    auto plan = placement.plan(shards, nodes, {});
    ASSERT_EQ(plan.assignments.size(), 2);
    EXPECT_EQ(plan.assignments[1], "node-a");
    EXPECT_EQ(plan.assignments[2], "node-b");
}

TEST(StaticPlacementTest, UnconfiguredShardIsUnassigned) {
    StaticPlacement placement;
    placement.set_mapping(1, "node-a");

    const auto shards = std::vector<ShardId>{1, 2};
    auto nodes = make_nodes({"node-a"});
    auto plan = placement.plan(shards, nodes, {});
    ASSERT_EQ(plan.assignments.size(), 1);
    EXPECT_EQ(plan.assignments[1], "node-a");
    // shard 2 not assigned — not in plan
    EXPECT_EQ(plan.assignments.find(2), plan.assignments.end());
}

TEST(StaticPlacementTest, OwnerNotInAliveNodesIsDropped) {
    StaticPlacement placement;
    placement.set_mapping(1, "node-dead");

    // node-dead is not in alive_nodes — should not be assigned
    const auto shards = std::vector<ShardId>{1};
    auto nodes = make_nodes({"node-alive"});
    auto plan = placement.plan(shards, nodes, {});
    EXPECT_TRUE(plan.assignments.empty());
}

// ── RendezvousHash tests ──

TEST(RendezvousHashTest, AllShardsAreAssigned) {
    RendezvousHash hash;
    auto shards = std::vector<ShardId>{0, 1, 2, 3, 4};
    auto nodes = make_nodes({"node-a", "node-b", "node-c"});

    auto plan = hash.plan(shards, nodes, {});
    EXPECT_EQ(plan.assignments.size(), 5); // all 5 shards assigned
}

TEST(RendezvousHashTest, DeterministicSameInputSameOutput) {
    RendezvousHash hash;
    auto shards = std::vector<ShardId>{0, 1, 2, 3, 4};
    auto nodes = make_nodes({"node-a", "node-b", "node-c"});

    auto plan1 = hash.plan(shards, nodes, {});
    auto plan2 = hash.plan(shards, nodes, {});

    for (auto shard : shards) {
        EXPECT_EQ(plan1.assignments[shard], plan2.assignments[shard]);
    }
}

TEST(RendezvousHashTest, EmptyNodesReturnsNoAssignments) {
    RendezvousHash hash;
    auto shards = std::vector<ShardId>{1, 2};
    auto plan = hash.plan(shards, {}, {});
    EXPECT_TRUE(plan.assignments.empty());
}

TEST(RendezvousHashTest, AddingNodeMinimizesMovement) {
    RendezvousHash hash;
    auto shards = std::vector<ShardId>{0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    auto nodes3 = make_nodes({"node-a", "node-b", "node-c"});
    auto nodes4 = make_nodes({"node-a", "node-b", "node-c", "node-d"});

    auto plan3 = hash.plan(shards, nodes3, {});
    auto plan4 = hash.plan(shards, nodes4, {});

    // Count how many shards moved
    int moved = 0;
    for (auto shard : shards) {
        auto it3 = plan3.assignments.find(shard);
        auto it4 = plan4.assignments.find(shard);
        if (it3 != plan3.assignments.end() && it4 != plan4.assignments.end()) {
            if (it3->second != it4->second)
                moved++;
        }
    }
    // With HRW, adding 1 node to 3 should move about 1/4 of shards
    EXPECT_LE(moved, 5); // at most ~half should move
}

TEST(RendezvousHashTest, EachShardAssignedToAliveNode) {
    RendezvousHash hash;
    auto shards = std::vector<ShardId>{0, 1, 2, 3, 4};
    auto nodes = make_nodes({"node-a", "node-b", "node-c"});

    auto plan = hash.plan(shards, nodes, {});

    for (const auto& [shard, owner] : plan.assignments) {
        bool found = false;
        for (const auto& node : nodes) {
            if (node == owner)
                found = true;
        }
        EXPECT_TRUE(found) << "Shard " << shard << " assigned to unknown node "
                           << owner;
    }
}

} // namespace hpactor::cluster::sharding
