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
#include <hpactor/cluster/sharding/shard_coordinator_actor.hpp>
#include <hpactor/cluster/sharding/static_placement.hpp>
#include <hpactor/cluster/singleton/leadership_lease.hpp>

namespace hpactor::cluster::sharding {

TEST(ShardCoordinatorActorTest, ConstructionSetsTotalShards) {
    auto strategy = std::make_unique<StaticPlacement>();
    ShardCoordinatorActor actor(16, std::move(strategy));
    EXPECT_EQ(actor.total_shards(), 16u);
}

TEST(ShardCoordinatorActorTest, RegisterActorAssignsShard) {
    auto strategy = std::make_unique<StaticPlacement>();
    ShardCoordinatorActor actor(16, std::move(strategy));
    LogicalActorId id{"order-1"};
    actor.register_actor(id, "node-1");
    EXPECT_TRUE(actor.core().is_actor_registered(id));
}

TEST(ShardCoordinatorActorTest, UnregisterActor) {
    auto strategy = std::make_unique<StaticPlacement>();
    ShardCoordinatorActor actor(16, std::move(strategy));
    LogicalActorId id{"order-1"};
    actor.register_actor(id, "node-1");
    actor.unregister_actor(id);
    EXPECT_FALSE(actor.core().is_actor_registered(id));
}

TEST(ShardCoordinatorActorTest, GetShardOwner) {
    auto strategy = std::make_unique<StaticPlacement>();
    ShardCoordinatorActor actor(16, std::move(strategy));
    actor.register_actor(LogicalActorId{"order-1"}, "node-1");
    ShardId shard = ShardResolver::resolve(LogicalActorId{"order-1"}, 16);
    EXPECT_EQ(actor.get_shard_owner(shard), "node-1");
}

TEST(ShardCoordinatorActorTokenTest, RebalanceWithValidTokenSucceeds) {
    auto strategy = std::make_unique<StaticPlacement>();
    ShardCoordinatorActor actor(16, std::move(strategy));

    singleton::LeadershipLease lease;
    lease.singleton_name = "shard-coordinator";
    lease.owner_node_id = "node-a";
    lease.fencing_token = 42;
    actor.on_lease_update(lease);

    EXPECT_TRUE(actor.rebalance_with_token({"node-a", "node-b"}, 42));
}

TEST(ShardCoordinatorActorTokenTest, RebalanceWithStaleTokenReturnsFalse) {
    auto strategy = std::make_unique<StaticPlacement>();
    ShardCoordinatorActor actor(16, std::move(strategy));

    singleton::LeadershipLease lease;
    lease.singleton_name = "shard-coordinator";
    lease.fencing_token = 42;
    actor.on_lease_update(lease);

    EXPECT_FALSE(actor.rebalance_with_token({"node-a"}, 10));
}

} // namespace hpactor::cluster::sharding
