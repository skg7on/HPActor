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
#include <hpactor/mailbox/reliable_retry_policy.hpp>

namespace hpactor::mailbox {

TEST(ReliableRetryPolicyTest, DefaultPolicyIsSensible) {
    ReliableRetryPolicy policy;
    EXPECT_EQ(policy.max_retries, 3);
    EXPECT_EQ(policy.initial_backoff, std::chrono::milliseconds(100));
    EXPECT_EQ(policy.max_backoff, std::chrono::seconds(10));
    EXPECT_DOUBLE_EQ(policy.backoff_multiplier, 2.0);
}

TEST(ReliableRetryPolicyTest, FirstRetryAfterInitialBackoff) {
    ReliableRetryPolicy policy;
    Duration delay = policy.backoff_for_attempt(1);
    EXPECT_EQ(delay, std::chrono::milliseconds(100));
}

TEST(ReliableRetryPolicyTest, SecondRetryDoubles) {
    ReliableRetryPolicy policy;
    Duration delay = policy.backoff_for_attempt(2);
    EXPECT_EQ(delay, std::chrono::milliseconds(200));
}

TEST(ReliableRetryPolicyTest, CapsAtMaxBackoff) {
    ReliableRetryPolicy policy;
    policy.max_backoff = std::chrono::milliseconds(500);
    Duration delay = policy.backoff_for_attempt(10);
    EXPECT_EQ(delay, std::chrono::milliseconds(500));
}

TEST(ReliableRetryPolicyTest, ShouldRetryWithinLimits) {
    ReliableRetryPolicy policy;
    EXPECT_TRUE(policy.should_retry(0));
    EXPECT_TRUE(policy.should_retry(2));
    EXPECT_FALSE(policy.should_retry(3));
}

} // namespace hpactor::mailbox
