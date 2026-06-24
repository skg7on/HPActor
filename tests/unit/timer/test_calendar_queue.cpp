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

// tests/unit/timer/test_calendar_queue.cpp

#include <chrono>
#include <functional>
#include <gtest/gtest.h>
#include <hpactor/adt/calendar_queue.hpp>
#include <memory>
#include <thread>

using namespace hpactor::adt;

static constexpr int64_t ONE_MS = 1'000'000;

// ---------------------------------------------------------------------------
// Helper: create a CalendarQueue with small fine_buckets so modulo-based
// collisions place timers with different expiry into the same bucket.
// ---------------------------------------------------------------------------
static CalendarQueueConfig small_fine_cfg() {
    CalendarQueueConfig cfg;
    cfg.fine_buckets = 4; // modulo wraps quickly
    cfg.coarse_buckets = 64;
    return cfg;
}

// ---------------------------------------------------------------------------
// Test fixture: CalendarQueue with 4 fine buckets.
// advance() initializes last_advance_ns_ so that schedule() / schedule_at()
// have a defined baseline.
// ---------------------------------------------------------------------------
class CalendarQueueTest : public ::testing::Test {
  protected:
    std::unique_ptr<CalendarQueue> queue_;
    int64_t t0_ = ONE_MS;

    CalendarQueueTest()
        : queue_(std::make_unique<CalendarQueue>(small_fine_cfg())) {}

    void SetUp() override {
        queue_->advance(t0_);
    }
};

// ---------------------------------------------------------------------------
// Bug regression: advance() must not destroy future timers in the
// same fine-wheel bucket.
//
// With fine_buckets=4, a timer at absolute 4 ms (t0_ + 3*ONE_MS) hashes to
// bucket 0 (4 & 3 == 0).  After SetUp, current_fine_ is also 0.
// Advancing to only 2 ms past t0 processes bucket 0 but the timer at 4 ms
// must NOT be destroyed — it must be re-inserted for later expiry.
// ---------------------------------------------------------------------------
TEST_F(CalendarQueueTest, FutureTimerSurvivesAdvance) {
    int fired = 0;

    // Timer at absolute 4 ms → fine bucket 0  (4 & 3 == 0).
    auto id = queue_->schedule_at(t0_ + 3 * ONE_MS, [&] { fired++; });
    ASSERT_NE(id, 0u);
    ASSERT_EQ(queue_->size(), 1u);

    // Advance to only 2 ms past t0.  The loop processes fine bucket 0
    // where the timer sits, but the timer has not expired yet (4 ms > 2 ms).
    uint32_t n = queue_->advance(t0_ + 2 * ONE_MS);

    EXPECT_EQ(n, 0u);
    EXPECT_EQ(fired, 0);
    EXPECT_EQ(queue_->size(), 1u);
    EXPECT_NE(queue_->next_deadline(), INT64_MAX);

    // Advance further past the actual expiry — timer should now fire.
    n = queue_->advance(t0_ + 5 * ONE_MS);
    EXPECT_EQ(n, 1u);
    EXPECT_EQ(fired, 1);
    EXPECT_EQ(queue_->size(), 0u);
    EXPECT_TRUE(queue_->empty());
}

// ---------------------------------------------------------------------------
// Basic schedule + advance
// ---------------------------------------------------------------------------
TEST_F(CalendarQueueTest, ScheduleAndFire) {
    int fired = 0;
    auto id = queue_->schedule(5 * ONE_MS, [&] { fired++; });
    EXPECT_GE(id, 1u);
    EXPECT_FALSE(queue_->empty());

    // Advance far enough past the expiry to guarantee the bucket is reached.
    uint32_t r = queue_->advance(t0_ + 20 * ONE_MS);
    EXPECT_GT(r, 0u);
    EXPECT_EQ(fired, 1);
    EXPECT_TRUE(queue_->empty());
}

// ---------------------------------------------------------------------------
// Cancel prevents timer from firing
// ---------------------------------------------------------------------------
TEST_F(CalendarQueueTest, CancelRemovesTimer) {
    int fired = 0;
    auto id = queue_->schedule(10 * ONE_MS, [&] { fired++; });
    EXPECT_TRUE(queue_->cancel(id));
    EXPECT_TRUE(queue_->empty());

    uint32_t r = queue_->advance(t0_ + 20 * ONE_MS);
    EXPECT_EQ(r, 0u);
    EXPECT_EQ(fired, 0);
}

// ---------------------------------------------------------------------------
// Empty queue metadata
// ---------------------------------------------------------------------------
TEST_F(CalendarQueueTest, EmptyAndNextDeadline) {
    EXPECT_TRUE(queue_->empty());
    EXPECT_EQ(queue_->next_deadline(), INT64_MAX);
    EXPECT_EQ(queue_->size(), 0u);
}

// ---------------------------------------------------------------------------
// TSAN stress: concurrent schedule() and advance() must not race on
// last_advance_ns_ or timer_map_.
// ---------------------------------------------------------------------------
TEST_F(CalendarQueueTest, ScheduleReadsTimeUnderLock) {
    std::atomic<bool> start{false};
    std::atomic<int> fired{0};
    std::atomic<int> scheduled{0};

    std::thread scheduler([&]() {
        while (!start.load()) { /* spin */
        }
        for (int i = 0; i < 1000; ++i) {
            static_cast<void>(
                queue_->schedule(1'000'000, [&fired]() { fired.fetch_add(1); }));
            scheduled.fetch_add(1);
        }
    });

    std::thread advancer([&]() {
        while (!start.load()) { /* spin */
        }
        int64_t t = queue_->current_time() + 1'000'000;
        for (int i = 0; i < 1000; ++i) {
            queue_->advance(t);
            t += 1'000'000;
            std::this_thread::sleep_for(std::chrono::microseconds(10));
        }
    });

    start.store(true);
    scheduler.join();
    advancer.join();

    // Advance enough to fire all remaining timers
    queue_->advance(queue_->current_time() + 5'000'000'000LL);

    // Verify no timers were lost
    EXPECT_EQ(static_cast<size_t>(fired.load()) + queue_->size(),
              static_cast<size_t>(scheduled.load()));
}
