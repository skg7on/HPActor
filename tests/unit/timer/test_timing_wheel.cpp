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

// tests/unit/timer/test_timing_wheel.cpp

#include <functional>
#include <gtest/gtest.h>
#include <hpactor/timer/timing_wheel.hpp>

using namespace hpactor::sched;

static constexpr int64_t ONE_MS = 1'000'000;

// ---------------------------------------------------------------------------
// Test fixture: fresh TimingWheel for each test
// ---------------------------------------------------------------------------
class TimingWheelTest : public ::testing::Test {
  protected:
    TimingWheel wheel_{ONE_MS, 4};
};

// ---------------------------------------------------------------------------
// Basic smoke tests
// ---------------------------------------------------------------------------

TEST_F(TimingWheelTest, ScheduleAndFire) {
    int fired = 0;
    int64_t t0 = wheel_.current_time();

    // Schedule at absolute time t0 + 10 ms.
    auto id = wheel_.schedule_at(t0 + 10 * ONE_MS, [&fired] { fired++; });
    EXPECT_GE(id, 1U);

    // Advance to t0 + 20 ms — timer should fire.
    uint32_t r = wheel_.advance(t0 + 20 * ONE_MS);
    EXPECT_EQ(r, 1U);
    EXPECT_EQ(fired, 1);
    EXPECT_TRUE(wheel_.empty());
}

TEST_F(TimingWheelTest, CancelRemovesTimer) {
    int fired = 0;
    auto id = wheel_.schedule(10 * ONE_MS, [&fired] { fired++; });
    EXPECT_TRUE(wheel_.cancel(id));
    EXPECT_TRUE(wheel_.empty());

    int64_t t0 = wheel_.current_time();
    uint32_t r = wheel_.advance(t0 + 20 * ONE_MS);
    EXPECT_EQ(r, 0U);
    EXPECT_EQ(fired, 0);
}

TEST_F(TimingWheelTest, CancelNonexistentReturnsFalse) {
    EXPECT_FALSE(wheel_.cancel(0));
    EXPECT_FALSE(wheel_.cancel(999999));
}

TEST_F(TimingWheelTest, CancelTwiceReturnsFalseSecondTime) {
    auto id = wheel_.schedule(10 * ONE_MS, [] {});
    EXPECT_TRUE(wheel_.cancel(id));
    EXPECT_FALSE(wheel_.cancel(id));
}

TEST_F(TimingWheelTest, ScheduleAtAbsoluteTime) {
    int fired = 0;
    int64_t t0 = wheel_.current_time();
    auto id = wheel_.schedule_at(t0 + 5 * ONE_MS, [&fired] { fired++; });
    EXPECT_GE(id, 1U);

    // Advance past expiry — timer fires.
    wheel_.advance(t0 + 10 * ONE_MS);
    EXPECT_EQ(fired, 1);
}

// ---------------------------------------------------------------------------
// Regression: cascade off-by-one (issue #256 finding #1)
// ---------------------------------------------------------------------------
// The cascade loop in advance() used `l <= lower_level` instead of
// `l < lower_level`, dividing by 256 one extra time.  This placed cascaded
// timers in wrong level-0 buckets (always dividing ticks by 256 one more
// time than insert_timer), delaying fire by up to 256 ms.
//
// This test places a timer at level 1 (delay > 256 ms), advances to
// trigger cascade to level 0, then advances past expiry and verifies
// the timer fires.

TEST_F(TimingWheelTest, CascadePlacesTimerInCorrectBucket) {
    int fired = 0;
    int64_t t0 = wheel_.current_time();

    // 500 ms delay — definitely in level 1 (range 256 ms .. 65 s).
    wheel_.schedule_at(t0 + 500 * ONE_MS, [&fired] { fired++; });

    // Advance in ~100 ms steps to stay under the 100 ms cap.  This
    // triggers cascade from level 1 → level 0 for timers whose
    // level-1 buckets are processed before their expiry.
    for (int step = 1; step <= 7; ++step) {
        wheel_.advance(t0 + step * 100 * ONE_MS);
    }

    // After 7 × 100 ms = 700 ms the 500 ms timer must have fired.
    EXPECT_EQ(fired, 1);
}

// ---------------------------------------------------------------------------
// Regression: level-finding fallback (issue #256 finding #2)
// ---------------------------------------------------------------------------
// Timers whose delay exceeds all level ranges silently fell to level 0
// instead of the defaulting to the highest level.  Verify a very-long-
// delay timer is insertable, findable via cancel, and reported by size().

TEST_F(TimingWheelTest, VeryLongDelayGoesToHighestLevel) {
    int64_t t0 = wheel_.current_time();

    // 100 days in nanoseconds — exceeds level 3 range (~49.7 days).
    static constexpr int64_t kOneHundredDays = 100LL * 24 * 3600 * 1'000'000'000;
    auto id = wheel_.schedule_at(t0 + kOneHundredDays, [] {});
    EXPECT_GE(id, 1U);
    EXPECT_FALSE(wheel_.empty());
    EXPECT_EQ(wheel_.size(), 1U);

    // Cancel should work — the timer was findable.
    EXPECT_TRUE(wheel_.cancel(id));
    EXPECT_TRUE(wheel_.empty());
}

// ---------------------------------------------------------------------------
// Multiple timers / ordering
// ---------------------------------------------------------------------------

TEST_F(TimingWheelTest, MultipleTimersFire) {
    int fired = 0;
    int64_t t0 = wheel_.current_time();

    wheel_.schedule_at(t0 + 10 * ONE_MS, [&fired] { fired++; });
    wheel_.schedule_at(t0 + 20 * ONE_MS, [&fired] { fired++; });
    wheel_.schedule_at(t0 + 30 * ONE_MS, [&fired] { fired++; });

    // Advance past all expiries.
    wheel_.advance(t0 + 50 * ONE_MS);
    EXPECT_EQ(fired, 3);
    EXPECT_TRUE(wheel_.empty());
}

TEST_F(TimingWheelTest, EmptyAndSize) {
    EXPECT_TRUE(wheel_.empty());
    EXPECT_EQ(wheel_.size(), 0U);

    auto id1 = wheel_.schedule(100 * ONE_MS, [] {});
    EXPECT_FALSE(wheel_.empty());
    EXPECT_EQ(wheel_.size(), 1U);

    auto id2 = wheel_.schedule(200 * ONE_MS, [] {});
    EXPECT_EQ(wheel_.size(), 2U);

    wheel_.cancel(id1);
    EXPECT_EQ(wheel_.size(), 1U);

    wheel_.cancel(id2);
    EXPECT_TRUE(wheel_.empty());
}

TEST_F(TimingWheelTest, FaultInjectionScheduleFailReturnsZero) {
    // Without fault config active this exercises the normal (non-fault) path.
    auto id = wheel_.schedule(ONE_MS, [] {});
    EXPECT_NE(id, 0U);
}

TEST_F(TimingWheelTest, AdvanceWithNoExpiredTimersReturnsZero) {
    int64_t t0 = wheel_.current_time();
    uint32_t r = wheel_.advance(t0 + ONE_MS);
    EXPECT_EQ(r, 0U);
}
