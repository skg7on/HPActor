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
#include <hpactor/cluster/singleton/leadership_status.hpp>

namespace hpactor::cluster::singleton {

TEST(LeadershipStatusTest, GrantedCarriesLease) {
    LeadershipLease lease;
    lease.singleton_name = "s1";
    lease.fencing_token = 42;
    LeadershipResult r = LeadershipResult::granted(lease);
    EXPECT_EQ(r.status, LeadershipStatusCode::Granted);
    ASSERT_TRUE(r.lease.has_value());
    EXPECT_EQ(r.lease->fencing_token, 42u);
}

TEST(LeadershipStatusTest, BackendUnavailableCarriesNoLease) {
    LeadershipResult r = LeadershipResult::unavailable();
    EXPECT_EQ(r.status, LeadershipStatusCode::BackendUnavailable);
    EXPECT_FALSE(r.lease.has_value());
}

TEST(LeadershipStatusTest, AlreadyOwnedCarriesCurrentOwner) {
    LeadershipLease lease;
    lease.owner_node_id = "node-b";
    LeadershipResult r = LeadershipResult::already_owned("node-b", lease);
    EXPECT_EQ(r.status, LeadershipStatusCode::AlreadyOwned);
    ASSERT_TRUE(r.current_owner.has_value());
    EXPECT_EQ(*r.current_owner, "node-b");
}

TEST(LeadershipStatusTest, LostCarriesReason) {
    LeadershipResult r = LeadershipResult::lost();
    EXPECT_EQ(r.status, LeadershipStatusCode::Lost);
    EXPECT_FALSE(r.lease.has_value());
}

TEST(LeadershipStatusTest, ReleasedIsSuccess) {
    LeadershipResult r = LeadershipResult::released();
    EXPECT_EQ(r.status, LeadershipStatusCode::Released);
}

TEST(LeadershipStatusTest, RenewedCarriesLease) {
    LeadershipLease lease;
    lease.fencing_token = 99;
    LeadershipResult r = LeadershipResult::renewed(lease);
    EXPECT_EQ(r.status, LeadershipStatusCode::Renewed);
    ASSERT_TRUE(r.lease.has_value());
    EXPECT_EQ(r.lease->fencing_token, 99u);
}

TEST(LeadershipStatusTest, AllStatusCodesHaveDistinctValues) {
    EXPECT_NE(static_cast<uint8_t>(LeadershipStatusCode::Granted),
              static_cast<uint8_t>(LeadershipStatusCode::Lost));
    EXPECT_NE(static_cast<uint8_t>(LeadershipStatusCode::BackendUnavailable),
              static_cast<uint8_t>(LeadershipStatusCode::TimedOut));
    EXPECT_NE(static_cast<uint8_t>(LeadershipStatusCode::IdentityRejected),
              static_cast<uint8_t>(LeadershipStatusCode::PermissionDenied));
}

TEST(LeadershipStatusTest, ToStringReturnsExpected) {
    EXPECT_STREQ(to_string(LeadershipStatusCode::Granted), "granted");
    EXPECT_STREQ(to_string(LeadershipStatusCode::Lost), "lost");
    EXPECT_STREQ(to_string(LeadershipStatusCode::BackendUnavailable),
                 "backend_unavailable");
}

} // namespace hpactor::cluster::singleton
