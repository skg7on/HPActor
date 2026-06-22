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
#include <hpactor/cluster/sharding/shard_table.hpp>

namespace hpactor::cluster::sharding {

class ShardTableTest : public ::testing::Test {
  protected:
    ShardTable table_;
};

TEST_F(ShardTableTest, NewTableHasZeroEpoch) {
    EXPECT_EQ(table_.epoch(), 0);
}

TEST_F(ShardTableTest, LookupMissingReturnsNullopt) {
    EXPECT_FALSE(table_.lookup(42).has_value());
}

TEST_F(ShardTableTest, UpdateThenLookupReturnsEntry) {
    table_.update(5, "node-3", 1);
    auto entry = table_.lookup(5);
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->shard_id, 5);
    EXPECT_EQ(entry->owner_node, "node-3");
    EXPECT_EQ(entry->epoch, 1);
}

TEST_F(ShardTableTest, UpdateWithLowerEpochIsIgnored) {
    table_.update(5, "node-3", 3);
    table_.update(5, "node-4", 2); // stale epoch — should be ignored
    auto entry = table_.lookup(5);
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->owner_node, "node-3"); // unchanged
    EXPECT_EQ(entry->epoch, 3);
}

TEST_F(ShardTableTest, UpdateWithHigherEpochOverwrites) {
    table_.update(5, "node-3", 1);
    table_.update(5, "node-7", 2);
    auto entry = table_.lookup(5);
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->owner_node, "node-7");
    EXPECT_EQ(entry->epoch, 2);
}

TEST_F(ShardTableTest, InvalidateForNodeRemovesEntries) {
    table_.update(1, "node-a", 1);
    table_.update(2, "node-a", 1);
    table_.update(3, "node-b", 1);

    table_.invalidate_for_node("node-a");

    EXPECT_FALSE(table_.lookup(1).has_value());
    EXPECT_FALSE(table_.lookup(2).has_value());
    EXPECT_TRUE(table_.lookup(3).has_value()); // node-b still there
}

TEST_F(ShardTableTest, ClearRemovesAllEntries) {
    table_.update(1, "node-a", 1);
    table_.update(2, "node-b", 1);
    table_.clear();

    EXPECT_FALSE(table_.lookup(1).has_value());
    EXPECT_FALSE(table_.lookup(2).has_value());
}

} // namespace hpactor::cluster::sharding
