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

#include <chrono>
#include <gtest/gtest.h>
#include <hpactor/msg/retry_policy.hpp>

using namespace hpactor::msg;
using namespace std::chrono;

TEST(RetryPolicyTest, DefaultDisabled) {
    RetryPolicy policy;
    EXPECT_FALSE(policy.is_enabled());
    EXPECT_EQ(policy.max_attempts, 1);
}

TEST(RetryPolicyTest, EnabledWhenMaxAttemptsAboveOne) {
    RetryPolicy policy;
    policy.max_attempts = 3;
    EXPECT_TRUE(policy.is_enabled());
}

TEST(RetryPolicyTest, FixedBackoffReturnsConstantDelay) {
    RetryPolicy policy;
    policy.backoff = RetryBackoff::Fixed;
    policy.initial_backoff = milliseconds(200);
    policy.jitter = false;
    EXPECT_EQ(policy.backoff_delay(1), milliseconds(200));
    EXPECT_EQ(policy.backoff_delay(2), milliseconds(200));
    EXPECT_EQ(policy.backoff_delay(5), milliseconds(200));
}

TEST(RetryPolicyTest, LinearBackoffScalesWithAttempt) {
    RetryPolicy policy;
    policy.backoff = RetryBackoff::Linear;
    policy.initial_backoff = milliseconds(100);
    policy.jitter = false;
    EXPECT_EQ(policy.backoff_delay(1), milliseconds(100));
    EXPECT_EQ(policy.backoff_delay(2), milliseconds(200));
    EXPECT_EQ(policy.backoff_delay(3), milliseconds(300));
}

TEST(RetryPolicyTest, ExponentialBackoffDoubles) {
    RetryPolicy policy;
    policy.backoff = RetryBackoff::Exponential;
    policy.initial_backoff = milliseconds(100);
    policy.jitter = false;
    EXPECT_EQ(policy.backoff_delay(1), milliseconds(100));
    EXPECT_EQ(policy.backoff_delay(2), milliseconds(200));
    EXPECT_EQ(policy.backoff_delay(3), milliseconds(400));
    EXPECT_EQ(policy.backoff_delay(4), milliseconds(800));
}

TEST(RetryPolicyTest, BackoffClampedToMax) {
    RetryPolicy policy;
    policy.backoff = RetryBackoff::Exponential;
    policy.initial_backoff = milliseconds(100);
    policy.max_backoff = milliseconds(500);
    policy.jitter = false;
    EXPECT_EQ(policy.backoff_delay(1), milliseconds(100));
    EXPECT_EQ(policy.backoff_delay(2), milliseconds(200));
    EXPECT_EQ(policy.backoff_delay(3), milliseconds(400));
    EXPECT_EQ(policy.backoff_delay(4), milliseconds(500));
    EXPECT_EQ(policy.backoff_delay(10), milliseconds(500));
}

TEST(RetryPolicyTest, JitterWithinBounds) {
    RetryPolicy policy;
    policy.backoff = RetryBackoff::Fixed;
    policy.initial_backoff = milliseconds(1000);
    policy.jitter = true;
    auto delay = policy.backoff_delay(1);
    EXPECT_GE(delay.count(), 750);
    EXPECT_LE(delay.count(), 1250);
}

TEST(RetryPolicyTest, NoJitterExact) {
    RetryPolicy policy;
    policy.backoff = RetryBackoff::Fixed;
    policy.initial_backoff = milliseconds(300);
    policy.jitter = false;
    EXPECT_EQ(policy.backoff_delay(1), milliseconds(300));
}
