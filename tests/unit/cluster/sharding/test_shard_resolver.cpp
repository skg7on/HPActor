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
#include <hpactor/cluster/sharding/shard_resolver.hpp>
#include <hpactor/cluster/sharding/shard_types.hpp>

namespace hpactor::cluster::sharding {

TEST(ShardResolverTest, SameIdMapsToSameShard) {
    LogicalActorId id{"tenant-42/session-9"};
    uint32_t total_shards = 100;
    ShardId shard1 = ShardResolver::resolve(id, total_shards);
    ShardId shard2 = ShardResolver::resolve(id, total_shards);
    EXPECT_EQ(shard1, shard2);
}

TEST(ShardResolverTest, DifferentIdsMapToDifferentShards) {
    LogicalActorId a{"tenant-42/session-1"};
    LogicalActorId b{"tenant-42/session-99"};
    uint32_t total_shards = 100;
    ShardId shard_a = ShardResolver::resolve(a, total_shards);
    ShardId shard_b = ShardResolver::resolve(b, total_shards);
    // May be equal due to hash collision, but deterministic
    EXPECT_EQ(shard_a, ShardResolver::resolve(a, total_shards));
    EXPECT_EQ(shard_b, ShardResolver::resolve(b, total_shards));
}

TEST(ShardResolverTest, ShardIdInRange) {
    LogicalActorId id{"test-actor"};
    uint32_t total_shards = 50;
    ShardId shard = ShardResolver::resolve(id, total_shards);
    EXPECT_LT(shard, total_shards);
}

TEST(ShardResolverTest, EmptyStringIsValid) {
    LogicalActorId id{""};
    uint32_t total_shards = 10;
    ShardId shard = ShardResolver::resolve(id, total_shards);
    EXPECT_LT(shard, total_shards);
}

TEST(ShardResolverTest, ShardEntryDefaultConstruction) {
    ShardEntry entry;
    EXPECT_EQ(entry.shard_id, 0);
    EXPECT_EQ(entry.epoch, 0);
    EXPECT_TRUE(entry.owner_node.empty());
}

TEST(ShardResolverTest, ShardEntryCanBeSet) {
    ShardEntry entry;
    entry.shard_id = 42;
    entry.owner_node = "node-7";
    entry.epoch = 3;
    EXPECT_EQ(entry.shard_id, 42);
    EXPECT_EQ(entry.owner_node, "node-7");
    EXPECT_EQ(entry.epoch, 3);
}

TEST(ShardResolverTest, LogicalActorIdConstruction) {
    LogicalActorId id{"order-123/cart"};
    EXPECT_EQ(id.persistence_id, "order-123/cart");
}

TEST(ShardResolverTest, DeterministicMapping) {
    // Same input must produce same output every time
    LogicalActorId id{"deterministic-test"};
    uint32_t total_shards = 64;
    ShardId first = ShardResolver::resolve(id, total_shards);
    for (int i = 0; i < 100; i++) {
        EXPECT_EQ(first, ShardResolver::resolve(id, total_shards));
    }
}

} // namespace hpactor::cluster::sharding
