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
#include <hpactor/cluster/partition_policy.hpp>

namespace hpactor::cluster {

TEST(PartitionPolicyTest, AllPoliciesDefined) {
    EXPECT_NE(static_cast<uint8_t>(PartitionPolicy::FailOpen),
              static_cast<uint8_t>(PartitionPolicy::FailClosed));
    EXPECT_NE(static_cast<uint8_t>(PartitionPolicy::FailClosed),
              static_cast<uint8_t>(PartitionPolicy::StaticMajority));
}

TEST(PartitionPolicyTest, FailOpenAllowsDelivery) {
    EXPECT_TRUE(allow_user_delivery(PartitionPolicy::FailOpen, false));
    EXPECT_TRUE(allow_user_delivery(PartitionPolicy::FailOpen, true));
}

TEST(PartitionPolicyTest, FailClosedBlocksWithoutQuorum) {
    EXPECT_FALSE(allow_user_delivery(PartitionPolicy::FailClosed, false));
    EXPECT_TRUE(allow_user_delivery(PartitionPolicy::FailClosed, true));
}

TEST(PartitionPolicyTest, FailClosedBlocksSingletonOwnershipWithoutQuorum) {
    EXPECT_FALSE(allow_ownership_change(PartitionPolicy::FailClosed, false));
}

TEST(PartitionPolicyTest, FailOpenAllowsOwnershipWithQuorum) {
    EXPECT_TRUE(allow_ownership_change(PartitionPolicy::FailOpen, true));
}

TEST(PartitionPolicyTest, StaticMajorityRequiresConfiguredMajority) {
    EXPECT_TRUE(allow_ownership_change(PartitionPolicy::StaticMajority, true));
    EXPECT_FALSE(allow_ownership_change(PartitionPolicy::StaticMajority, false));
}

TEST(PartitionPolicyTest, ToStringReturnsExpected) {
    EXPECT_STREQ(to_string(PartitionPolicy::FailOpen), "fail_open");
    EXPECT_STREQ(to_string(PartitionPolicy::FailClosed), "fail_closed");
    EXPECT_STREQ(to_string(PartitionPolicy::StaticMajority), "static_majority");
}

} // namespace hpactor::cluster
