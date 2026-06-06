// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>
#include <hpactor/msg/request_timeout.hpp>

namespace hpactor {
namespace {

TEST(RequestTimeoutTest, DefaultConstructionIsUseDefault) {
    RequestTimeout t;
    EXPECT_TRUE(t.is_default());
    EXPECT_EQ(t.kind, RequestTimeout::Kind::Duration);
    EXPECT_EQ(t.value.count(), 0);
}

TEST(RequestTimeoutTest, FromMsCreatesDurationKind) {
    auto t = RequestTimeout::from_ms(3000);
    EXPECT_EQ(t.kind, RequestTimeout::Kind::Duration);
    EXPECT_EQ(t.value.count(), 3000);
    EXPECT_FALSE(t.is_default());
}

TEST(RequestTimeoutTest, FromMsZeroIsDefault) {
    auto t = RequestTimeout::from_ms(0);
    EXPECT_TRUE(t.is_default());
}

TEST(RequestTimeoutTest, FromDeadlineCreatesDeadlineKind) {
    auto tp = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    auto t = RequestTimeout::from_deadline(tp);
    EXPECT_EQ(t.kind, RequestTimeout::Kind::Deadline);
    EXPECT_FALSE(t.is_default());
}

TEST(RequestTimeoutTest, UseDefaultIsZeroDuration) {
    auto t = RequestTimeout::use_default();
    EXPECT_TRUE(t.is_default());
}

TEST(RequestTimeoutTest, DurationDeadlineIsInFuture) {
    auto t = RequestTimeout::from_ms(5000);
    auto now = std::chrono::steady_clock::now();
    auto d = t.deadline();
    EXPECT_GT(d, now);
    EXPECT_GT(d - now, std::chrono::milliseconds(4000));
    EXPECT_LT(d - now, std::chrono::milliseconds(6000));
}

TEST(RequestTimeoutTest, DeadlineKindPreservesAbsolutePoint) {
    auto tp = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        tp.time_since_epoch());
    auto t = RequestTimeout::from_deadline(tp);
    auto d = t.deadline();
    auto d_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        d.time_since_epoch());
    EXPECT_GE(d_ms.count(), ms.count() - 1);
    EXPECT_LE(d_ms.count(), ms.count() + 1);
}

TEST(RequestTimeoutTest, DefaultDeadlineIsMax) {
    auto t = RequestTimeout::use_default();
    EXPECT_EQ(t.deadline(), std::chrono::steady_clock::time_point::max());
}

} // namespace
} // namespace hpactor
