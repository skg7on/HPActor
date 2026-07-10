// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>
#include <hpactor/cluster/singleton/fake_leadership_backend.hpp>
#include <hpactor/cluster/singleton/leadership_backend_adapter.hpp>

namespace hpactor::cluster::singleton {

using namespace std::chrono_literals;

class LeadershipBackendAdapterTest : public ::testing::Test {
  protected:
    void SetUp() override {
        backend_ = std::make_unique<FakeLeadershipBackend>();
    }
    std::unique_ptr<FakeLeadershipBackend> backend_;
};

TEST_F(LeadershipBackendAdapterTest, ElectReturnsOwnerWhenBackendGrantsLease) {
    backend_->force_grant("shard-coordinator", "node-self", 10s);
    LeadershipBackendAdapter adapter("node-self", backend_.get());

    SingletonIdentity id{"shard-coordinator", 0};
    std::vector<std::string> alive = {"node-self", "node-other"};
    auto winner = adapter.elect(id, alive);

    ASSERT_TRUE(winner.has_value());
    EXPECT_EQ(*winner, "node-self");
}

TEST_F(LeadershipBackendAdapterTest, ElectReturnsOtherOwnerWhenNotSelf) {
    backend_->force_grant("shard-coordinator", "node-other", 10s);
    LeadershipBackendAdapter adapter("node-self", backend_.get());

    SingletonIdentity id{"shard-coordinator", 0};
    std::vector<std::string> alive = {"node-self", "node-other"};
    auto winner = adapter.elect(id, alive);

    ASSERT_TRUE(winner.has_value());
    EXPECT_EQ(*winner, "node-other");
}

TEST_F(LeadershipBackendAdapterTest, ElectReturnsNulloptWhenNoOwner) {
    LeadershipBackendAdapter adapter("node-self", backend_.get());

    SingletonIdentity id{"shard-coordinator", 0};
    std::vector<std::string> alive = {"node-self"};
    auto winner = adapter.elect(id, alive);

    EXPECT_FALSE(winner.has_value());
}

TEST_F(LeadershipBackendAdapterTest, ElectReturnsNulloptWhenBackendUnavailable) {
    backend_->simulate_unavailable(true);
    LeadershipBackendAdapter adapter("node-self", backend_.get());

    SingletonIdentity id{"shard-coordinator", 0};
    std::vector<std::string> alive = {"node-self"};
    auto winner = adapter.elect(id, alive);

    EXPECT_FALSE(winner.has_value());
}

TEST_F(LeadershipBackendAdapterTest, GetFencingTokenReturnsBackendToken) {
    backend_->force_grant("shard-coordinator", "node-self", 10s);
    LeadershipBackendAdapter adapter("node-self", backend_.get());

    SingletonIdentity id{"shard-coordinator", 0};
    std::vector<std::string> alive = {"node-self"};
    adapter.elect(id, alive);

    auto token = adapter.get_fencing_token("shard-coordinator");
    EXPECT_GT(token, 0u);
}

TEST_F(LeadershipBackendAdapterTest, GetFencingTokenReturnsZeroWhenNoLease) {
    LeadershipBackendAdapter adapter("node-self", backend_.get());
    EXPECT_EQ(adapter.get_fencing_token("shard-coordinator"), 0u);
}

TEST_F(LeadershipBackendAdapterTest, GetFencingTokenReturnsZeroForUnknownSingleton) {
    backend_->force_grant("shard-coordinator", "node-self", 10s);
    LeadershipBackendAdapter adapter("node-self", backend_.get());

    SingletonIdentity id{"shard-coordinator", 0};
    std::vector<std::string> alive = {"node-self"};
    adapter.elect(id, alive);

    EXPECT_EQ(adapter.get_fencing_token("other-singleton"), 0u);
}

TEST_F(LeadershipBackendAdapterTest, OnPeerDownIsNoOp) {
    LeadershipBackendAdapter adapter("node-self", backend_.get());
    // Should not throw or crash — backend handles fencing
    adapter.on_peer_down("node-other");
    SUCCEED();
}

} // namespace hpactor::cluster::singleton
