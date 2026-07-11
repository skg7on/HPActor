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
#include <hpactor/cluster/singleton/fake_leadership_backend.hpp>

#include <chrono>

namespace hpactor::cluster::singleton {

using namespace std::chrono_literals;

class FakeLeadershipBackendTest : public ::testing::Test {
  protected:
    void SetUp() override {
        backend_ = std::make_unique<FakeLeadershipBackend>();
    }
    std::unique_ptr<FakeLeadershipBackend> backend_;
};

TEST_F(FakeLeadershipBackendTest, TryAcquireReturnsGrantedWhenNoOwner) {
    backend_->force_grant("s1", "node-a", 10s);

    LeadershipAttempt attempt;
    attempt.singleton_name = "s1";
    attempt.self_node_id = "node-a";
    attempt.lease_ttl = 10s;

    auto result = backend_->try_acquire(attempt);
    EXPECT_EQ(result.status, LeadershipStatusCode::Granted);
    ASSERT_TRUE(result.lease.has_value());
    EXPECT_EQ(result.lease->owner_node_id, "node-a");
    EXPECT_EQ(result.lease->singleton_name, "s1");
    EXPECT_GT(result.lease->fencing_token, 0u);
}

TEST_F(FakeLeadershipBackendTest, TryAcquireReturnsAlreadyOwnedForDifferentCaller) {
    backend_->force_grant("s1", "node-a", 10s);

    // node-b tries to acquire — already owned by node-a
    LeadershipAttempt attempt;
    attempt.singleton_name = "s1";
    attempt.self_node_id = "node-b";
    attempt.lease_ttl = 10s;

    auto result = backend_->try_acquire(attempt);
    EXPECT_EQ(result.status, LeadershipStatusCode::AlreadyOwned);
    ASSERT_TRUE(result.current_owner.has_value());
    EXPECT_EQ(*result.current_owner, "node-a");
}

TEST_F(FakeLeadershipBackendTest, RenewReturnsRenewedWhenTokenMatches) {
    backend_->force_grant("s1", "node-a", 10s);

    LeadershipAttempt attempt;
    attempt.singleton_name = "s1";
    attempt.self_node_id = "node-a";
    attempt.lease_ttl = 10s;
    auto acquired = backend_->try_acquire(attempt);
    ASSERT_TRUE(acquired.lease.has_value());

    auto result = backend_->renew(*acquired.lease);
    EXPECT_EQ(result.status, LeadershipStatusCode::Renewed);
    ASSERT_TRUE(result.lease.has_value());
    EXPECT_GT(result.lease->fencing_token, acquired.lease->fencing_token);
}

TEST_F(FakeLeadershipBackendTest, ReleaseFreesOwnership) {
    backend_->force_grant("s1", "node-a", 10s);

    LeadershipAttempt attempt;
    attempt.singleton_name = "s1";
    attempt.self_node_id = "node-a";
    attempt.lease_ttl = 10s;
    auto acquired = backend_->try_acquire(attempt);
    ASSERT_TRUE(acquired.lease.has_value());

    auto rel = backend_->release(*acquired.lease);
    EXPECT_EQ(rel.status, LeadershipStatusCode::Released);

    // Now someone else can acquire
    LeadershipAttempt attempt2{"s1", "node-b", 0, 10s};
    auto result2 = backend_->try_acquire(attempt2);
    EXPECT_EQ(result2.status, LeadershipStatusCode::Granted);
}

TEST_F(FakeLeadershipBackendTest, CurrentOwnerReturnsOwnerAfterGrant) {
    backend_->force_grant("s1", "node-a", 10s);

    LeadershipAttempt attempt{"s1", "node-a", 0, 10s};
    backend_->try_acquire(attempt);

    auto result = backend_->current_owner("s1");
    EXPECT_EQ(result.status, LeadershipStatusCode::Granted);
    ASSERT_TRUE(result.lease.has_value());
    EXPECT_EQ(result.lease->owner_node_id, "node-a");
}

TEST_F(FakeLeadershipBackendTest, CurrentOwnerReturnsNotOwnerWhenUnset) {
    auto result = backend_->current_owner("nonexistent");
    EXPECT_EQ(result.status, LeadershipStatusCode::NotOwner);
    EXPECT_FALSE(result.lease.has_value());
}

TEST_F(FakeLeadershipBackendTest, SimulateUnavailableReturnsBackendUnavailable) {
    backend_->simulate_unavailable(true);

    LeadershipAttempt attempt{"s1", "node-a", 0, 10s};
    auto result = backend_->try_acquire(attempt);
    EXPECT_EQ(result.status, LeadershipStatusCode::BackendUnavailable);
}

TEST_F(FakeLeadershipBackendTest, ForceRevokeClearsOwnership) {
    backend_->force_grant("s1", "node-a", 10s);

    LeadershipAttempt attempt{"s1", "node-a", 0, 10s};
    backend_->try_acquire(attempt);

    backend_->force_revoke("s1");

    auto result = backend_->current_owner("s1");
    EXPECT_EQ(result.status, LeadershipStatusCode::NotOwner);
}

} // namespace hpactor::cluster::singleton
