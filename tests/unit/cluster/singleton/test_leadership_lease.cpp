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
#include <hpactor/cluster/singleton/leadership_lease.hpp>

#include <chrono>

namespace hpactor::cluster::singleton {

using Clock = std::chrono::steady_clock;

LeadershipLease
make_lease(const std::string& singleton_name, const std::string& owner,
           uint64_t token, Clock::time_point deadline) {
    LeadershipLease l;
    l.cluster_id = "test-cluster";
    l.singleton_name = singleton_name;
    l.owner_node_id = owner;
    l.owner_incarnation = 1;
    l.owner_process_start_id = 100;
    l.membership_epoch = 1;
    l.fencing_token = token;
    l.backend_term = 0;
    l.backend_revision = token;
    l.lease_deadline = deadline;
    return l;
}

TEST(LeadershipLeaseTest, DefaultConstructionHasZeroToken) {
    LeadershipLease lease;
    EXPECT_TRUE(lease.cluster_id.empty());
    EXPECT_EQ(lease.fencing_token, 0u);
}

TEST(LeadershipLeaseTest, HigherTokenIsGreater) {
    auto now = Clock::now();
    auto deadline = now + std::chrono::seconds(10);
    auto a = make_lease("s1", "node-a", 1, deadline);
    auto b = make_lease("s1", "node-b", 2, deadline);
    EXPECT_LT(a, b);
    EXPECT_GT(b, a);
    EXPECT_NE(a, b);
}

TEST(LeadershipLeaseTest, SameTokenDifferentSingletonNotComparable) {
    auto now = Clock::now();
    auto deadline = now + std::chrono::seconds(10);
    auto a = make_lease("s1", "node-a", 5, deadline);
    auto b = make_lease("s2", "node-b", 5, deadline);
    // Same token, different singleton — comparison is by singleton_name then
    // token
    EXPECT_NE(a, b);
}

TEST(LeadershipLeaseTest, FencesReturnsTrueWhenTokenGreater) {
    auto now = Clock::now();
    auto deadline = now + std::chrono::seconds(10);
    auto old_lease = make_lease("s1", "node-a", 3, deadline);
    auto new_lease = make_lease("s1", "node-b", 7, deadline);
    EXPECT_TRUE(new_lease.fences(old_lease));
    EXPECT_FALSE(old_lease.fences(new_lease));
}

TEST(LeadershipLeaseTest, FencesReturnsFalseWhenSameToken) {
    auto now = Clock::now();
    auto deadline = now + std::chrono::seconds(10);
    auto a = make_lease("s1", "node-a", 5, deadline);
    auto b = make_lease("s1", "node-b", 5, deadline);
    EXPECT_FALSE(b.fences(a));
}

TEST(LeadershipLeaseTest, IsExpiredReturnsTrueAfterDeadline) {
    auto past = Clock::now() - std::chrono::seconds(1);
    auto l = make_lease("s1", "node-a", 1, past);
    EXPECT_TRUE(l.is_expired(Clock::now()));
}

TEST(LeadershipLeaseTest, IsExpiredReturnsFalseBeforeDeadline) {
    auto future = Clock::now() + std::chrono::seconds(60);
    auto l = make_lease("s1", "node-a", 1, future);
    EXPECT_FALSE(l.is_expired(Clock::now()));
}

TEST(LeadershipLeaseTest, OperatorLessEqual) {
    auto now = Clock::now();
    auto deadline = now + std::chrono::seconds(10);
    auto a = make_lease("s1", "node-a", 1, deadline);
    auto b = make_lease("s1", "node-a", 1, deadline);
    EXPECT_LE(a, b);
    auto c = make_lease("s1", "node-b", 2, deadline);
    EXPECT_LE(a, c);
}

} // namespace hpactor::cluster::singleton
