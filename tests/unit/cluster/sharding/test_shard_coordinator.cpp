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
#include <hpactor/cluster/sharding/rendezvous_hash.hpp>
#include <hpactor/cluster/sharding/shard_coordinator.hpp>
#include <hpactor/cluster/sharding/shard_resolver.hpp>

namespace hpactor::cluster::sharding {

class ShardCoordinatorTest : public ::testing::Test {
  protected:
    void SetUp() override {
        coordinator_ = std::make_unique<ShardCoordinatorCore>(
            100, std::make_unique<RendezvousHash>());
    }
    std::unique_ptr<ShardCoordinatorCore> coordinator_;
};

TEST_F(ShardCoordinatorTest, TotalShardsIsCorrect) {
    EXPECT_EQ(coordinator_->total_shards(), 100);
}

TEST_F(ShardCoordinatorTest, RegisterActorAssignsShard) {
    LogicalActorId id{"tenant-1/session-a"};
    coordinator_->register_actor(id, "node-1");

    ShardId expected_shard = ShardResolver::resolve(id, 100);
    auto owner = coordinator_->get_shard_owner(expected_shard);
    EXPECT_EQ(owner, "node-1");
}

TEST_F(ShardCoordinatorTest, UnregisterActorClearsAssignment) {
    LogicalActorId id{"tenant-1/session-a"};
    coordinator_->register_actor(id, "node-1");
    coordinator_->unregister_actor(id);

    // After unregister, the actor is no longer tracked
    EXPECT_FALSE(coordinator_->is_actor_registered(id));
}

TEST_F(ShardCoordinatorTest, RebalanceAssignsAllShards) {
    std::vector<std::string> nodes = {"node-a", "node-b", "node-c"};
    coordinator_->rebalance(nodes);

    // All 100 shards should be assigned
    for (uint32_t i = 0; i < 100; i++) {
        auto owner = coordinator_->get_shard_owner(i);
        EXPECT_FALSE(owner.empty()) << "Shard " << i << " is unassigned";
    }
}

TEST_F(ShardCoordinatorTest, NodeRemovedTriggersRebalance) {
    std::vector<std::string> nodes_before = {"node-a", "node-b", "node-c"};
    coordinator_->rebalance(nodes_before);

    // Remove node-b
    std::vector<std::string> nodes_after = {"node-a", "node-c"};
    coordinator_->rebalance(nodes_after);

    // No shard should be assigned to the removed node
    for (uint32_t i = 0; i < 100; i++) {
        auto owner = coordinator_->get_shard_owner(i);
        EXPECT_NE(owner, "node-b")
            << "Shard " << i << " still assigned to removed node";
    }
}

TEST_F(ShardCoordinatorTest, EpochIncrementsOnRebalance) {
    uint64_t epoch_before = coordinator_->epoch();

    std::vector<std::string> nodes = {"node-a"};
    coordinator_->rebalance(nodes);

    EXPECT_GT(coordinator_->epoch(), epoch_before);
}

TEST_F(ShardCoordinatorTest, GetShardTableReturnsCurrentState) {
    std::vector<std::string> nodes = {"node-x", "node-y"};
    coordinator_->rebalance(nodes);

    const auto& table = coordinator_->shard_table();
    EXPECT_GT(table.epoch(), 0);
}

TEST_F(ShardCoordinatorTest, EmptyNodeListClearsAll) {
    coordinator_->rebalance({"node-a", "node-b"});
    coordinator_->rebalance({});

    // No shards assigned when no nodes are alive
    for (uint32_t i = 0; i < 100; i++) {
        auto owner = coordinator_->get_shard_owner(i);
        EXPECT_TRUE(owner.empty());
    }
}

TEST_F(ShardCoordinatorTest, GetShardOwnerUnassignedReturnsEmpty) {
    auto owner = coordinator_->get_shard_owner(999); // nonexistent shard
    EXPECT_TRUE(owner.empty());
}

TEST_F(ShardCoordinatorTest, RepeatedRegistrationIsIdempotent) {
    LogicalActorId id{"test-actor"};
    coordinator_->register_actor(id, "node-1");
    ShardId shard = ShardResolver::resolve(id, 100);
    std::string first_owner = coordinator_->get_shard_owner(shard);

    // Register again — should not change the assignment
    coordinator_->register_actor(id, "node-2");
    EXPECT_EQ(coordinator_->get_shard_owner(shard), first_owner);
}

} // namespace hpactor::cluster::sharding
