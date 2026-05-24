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
#include <hpactor/mailbox/mailbox_policy.hpp>

#include <cstdint>

using namespace hpactor::mailbox;

TEST(IsExpiredTest, ExpiredWhenDeadlineBeforeNow) {
    EXPECT_TRUE(is_expired(100, 200));
}

TEST(IsExpiredTest, NotExpiredWhenDeadlineAfterNow) {
    EXPECT_FALSE(is_expired(200, 100));
}

TEST(IsExpiredTest, NotExpiredWhenNoDeadline) {
    EXPECT_FALSE(is_expired(-1, 100));
    EXPECT_FALSE(is_expired(-1, 0));
    EXPECT_FALSE(is_expired(-1, UINT64_MAX));
}

TEST(IsExpiredTest, NotExpiredWhenMaxDeadline) {
    EXPECT_FALSE(is_expired(INT64_MAX, 0));
    EXPECT_FALSE(is_expired(INT64_MAX, UINT64_MAX));
}

TEST(IsExpiredTest, BoundaryExclusive) {
    // deadline_ns == now_ns → not expired (boundary-inclusive)
    EXPECT_FALSE(is_expired(100, 100));
    // deadline_ns == now_ns - 1 → expired
    EXPECT_TRUE(is_expired(100, 101));
}

TEST(IsExpiredTest, ZeroDeadline) {
    // A deadline of 0 with now=0 is not expired (boundary)
    EXPECT_FALSE(is_expired(0, 0));
    // A deadline of 0 with now=1 is expired
    EXPECT_TRUE(is_expired(0, 1));
}

TEST(IsExpiredTest, LargeValues) {
    EXPECT_FALSE(is_expired(1000000000000LL, 999999999999ULL));
    EXPECT_TRUE(is_expired(999999999999LL, 1000000000000ULL));
}
