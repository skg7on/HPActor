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
#include <hpactor/cluster/sharding/shard_coordinator.hpp>
#include <hpactor/cluster/sharding/static_placement.hpp>
#include <hpactor/cluster/singleton/leadership_lease.hpp>

namespace hpactor::cluster::sharding {

class ShardCoordinatorTokenTest : public ::testing::Test {
  protected:
    void SetUp() override {
        auto strategy = std::make_unique<StaticPlacement>();
        coordinator_ =
            std::make_unique<ShardCoordinatorCore>(16, std::move(strategy));
    }
    std::unique_ptr<ShardCoordinatorCore> coordinator_;
};

singleton::LeadershipLease make_lease(const std::string& owner, uint64_t token) {
    singleton::LeadershipLease l;
    l.singleton_name = "shard-coordinator";
    l.owner_node_id = owner;
    l.fencing_token = token;
    return l;
}

TEST_F(ShardCoordinatorTokenTest, ValidateRejectsWhenNoLeaseSet) {
    EXPECT_FALSE(coordinator_->validate_token(1, "shard-coordinator"));
}

TEST_F(ShardCoordinatorTokenTest, ValidateAcceptsActiveLeaseToken) {
    auto lease = make_lease("node-a", 5);
    coordinator_->set_active_lease(lease);
    EXPECT_TRUE(coordinator_->validate_token(5, "shard-coordinator"));
}

TEST_F(ShardCoordinatorTokenTest, ValidateRejectsStaleToken) {
    auto lease = make_lease("node-a", 10);
    coordinator_->set_active_lease(lease);
    EXPECT_FALSE(coordinator_->validate_token(5, "shard-coordinator"));
}

TEST_F(ShardCoordinatorTokenTest, ValidateRejectsHigherTokenFromDifferentLease) {
    auto lease = make_lease("node-a", 10);
    coordinator_->set_active_lease(lease);
    // Token 15 but singleton name is different — should reject
    EXPECT_FALSE(coordinator_->validate_token(15, "other-singleton"));
}

TEST_F(ShardCoordinatorTokenTest, ClearActiveLeaseResetsValidation) {
    auto lease = make_lease("node-a", 5);
    coordinator_->set_active_lease(lease);
    coordinator_->clear_active_lease();
    EXPECT_FALSE(coordinator_->validate_token(5, "shard-coordinator"));
}

} // namespace hpactor::cluster::sharding
