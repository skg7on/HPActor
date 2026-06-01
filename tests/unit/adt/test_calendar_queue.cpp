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

// tests/unit/adt/test_calendar_queue.cpp

#include <functional>
#include <gtest/gtest.h>
#include <hpactor/adt/calendar_queue.hpp>

using namespace hpactor::adt;

static constexpr int64_t ONE_MS = 1'000'000;

// ---------------------------------------------------------------------------
// Test fixture: fresh CalendarQueue for each test
// ---------------------------------------------------------------------------
class CalendarQueueTest : public ::testing::Test {
  protected:
    CalendarQueue q_;
};

TEST_F(CalendarQueueTest, BasicSchedule) {
    int fired = 0;
    auto id = q_.schedule(ONE_MS, [&fired] { fired++; });
    EXPECT_GE(id, 1U);
    uint32_t r1 = q_.advance(ONE_MS); // init: sets clock origin
    EXPECT_EQ(r1, 0U);
    uint32_t r2 = q_.advance(3 * ONE_MS); // advance through bucket containing
                                          // timer
    EXPECT_EQ(r2, 1U);
    EXPECT_EQ(fired, 1);
    EXPECT_EQ(q_.size(), 0U);
}

TEST_F(CalendarQueueTest, Cancel) {
    int fired = 0;
    auto id = q_.schedule(ONE_MS, [&fired] { fired++; });
    bool ok = q_.cancel(id);
    EXPECT_TRUE(ok);
    EXPECT_EQ(q_.size(), 0U);
    q_.advance(ONE_MS);
    uint32_t r = q_.advance(3 * ONE_MS);
    EXPECT_EQ(r, 0U);
    EXPECT_EQ(fired, 0);
}

TEST_F(CalendarQueueTest, CancelNonexistent) {
    EXPECT_FALSE(q_.cancel(999));
    EXPECT_FALSE(q_.cancel(0));
}

TEST_F(CalendarQueueTest, ZeroDelayClamped) {
    int fired = 0;
    auto id = q_.schedule(0, [&fired] { fired++; }); // zero delay -> clamped to
                                                     // 1 fine bucket
    EXPECT_GE(id, 1U);
    q_.advance(ONE_MS);
    uint32_t r = q_.advance(3 * ONE_MS);
    EXPECT_EQ(r, 1U);
    EXPECT_EQ(fired, 1);
}

TEST_F(CalendarQueueTest, FineCoarseSplit) {
    // fine = 8 * 1 ms = 8 ms,  coarse = 8 * 8 ms = 64 ms
    CalendarQueueConfig cfg{ONE_MS, 8, 8, 8, 4096};
    CalendarQueue q(cfg);
    int fired = 0;

    // 10 ms delay  ->  10 > 8 (fine) and 10 < 64 (coarse)  ->  coarse wheel
    auto id = q.schedule(10 * ONE_MS, [&fired] { fired++; });
    EXPECT_GE(id, 1U);

    // Advance in 1 ms steps.  The timer cascades from coarse -> fine when
    // the fine wheel wraps enough times to reach the correct coarse bucket.
    for (int64_t t = ONE_MS; t <= 30 * ONE_MS; t += ONE_MS) {
        q.advance(t);
        if (fired > 0)
            break;
    }
    EXPECT_EQ(fired, 1);
}

TEST_F(CalendarQueueTest, CascadeCoarse) {
    CalendarQueueConfig cfg{ONE_MS, 8, 8, 8, 4096};
    CalendarQueue q(cfg);
    int fired = 0;

    for (int i = 0; i < 5; i++) {
        auto id = q.schedule(10 * ONE_MS, [&fired] { fired++; });
        EXPECT_GE(id, 1U);
    }
    EXPECT_EQ(q.size(), 5U);

    for (int64_t t = ONE_MS; t <= 30 * ONE_MS; t += ONE_MS) {
        q.advance(t);
        if (fired == 5)
            break;
    }
    EXPECT_EQ(fired, 5);
    EXPECT_EQ(q.size(), 0U);
}

TEST_F(CalendarQueueTest, RemoteCascade) {
    // fine = 4 * 1 ms = 4 ms, coarse = 4 * 4 ms = 16 ms, remote = 4 * 16 ms =
    // 64 ms
    CalendarQueueConfig cfg{ONE_MS, 4, 4, 4, 4096};
    CalendarQueue q(cfg);
    int fired = 0;

    // 30 ms  ->  30 > 16 (coarse) and 30 < 64  ->  remote wheel
    auto id = q.schedule(30 * ONE_MS, [&fired] { fired++; });
    EXPECT_GE(id, 1U);

    // Advance in 1 ms steps through enough time for the timer to cascade
    // from remote -> coarse -> fine and fire.
    for (int64_t t = ONE_MS; t <= 60 * ONE_MS; t += ONE_MS) {
        q.advance(t);
        if (fired > 0)
            break;
    }
    EXPECT_EQ(fired, 1);
}

TEST_F(CalendarQueueTest, RecurringNoDeadlock) {
    CalendarQueue q;
    int count = 0;

    std::function<void()> cb;
    cb = [&]() {
        count++;
        if (count < 5) {
            auto rid = q.schedule(ONE_MS, cb);
            (void)rid;
        }
    };

    q.advance(1); // init clock
    auto id = q.schedule(ONE_MS, cb);
    EXPECT_GE(id, 1U);

    int64_t t = 2 * ONE_MS;
    while (count < 5) {
        q.advance(t);
        t += ONE_MS;
    }
    EXPECT_EQ(count, 5);
}

TEST_F(CalendarQueueTest, CancelDuringAdvance) {
    CalendarQueue q;
    int fired_a = 0;
    int fired_b = 0;
    uint64_t id_b = 0;

    auto cb_a = [&]() {
        fired_a++;
        q.cancel(id_b);
    };
    auto cb_b = [&]() { fired_b++; };

    auto id_a = q.schedule(ONE_MS, cb_a); // A at bucket 1
    EXPECT_GE(id_a, 1U);
    id_b = q.schedule(2 * ONE_MS, cb_b); // B at bucket 2
    EXPECT_GE(id_b, 1U);
    q.advance(ONE_MS); // init
    uint32_t r = q.advance(3 * ONE_MS);
    EXPECT_EQ(r, 1U);
    EXPECT_EQ(fired_a, 1);
    EXPECT_EQ(fired_b, 0);
}

TEST_F(CalendarQueueTest, TimeJump) {
    CalendarQueue q;
    int fired = 0;
    auto id1 = q.schedule(1 * ONE_MS, [&fired] { fired++; });
    auto id2 = q.schedule(2 * ONE_MS, [&fired] { fired++; });
    auto id3 = q.schedule(3 * ONE_MS, [&fired] { fired++; });
    EXPECT_GE(id1, 1U);
    EXPECT_GE(id2, 1U);
    EXPECT_GE(id3, 1U);
    q.advance(ONE_MS);                  // init
    uint32_t r = q.advance(5 * ONE_MS); // should fire all 3
    EXPECT_EQ(r, 3U);
    EXPECT_EQ(fired, 3);
}

TEST_F(CalendarQueueTest, TimeBackwards) {
    CalendarQueue q;
    q.advance(5 * ONE_MS);              // init, last_advance = 5 ms
    q.advance(6 * ONE_MS);              // advance normally
    uint32_t r = q.advance(3 * ONE_MS); // backwards -- must return 0
    EXPECT_EQ(r, 0U);
}

TEST_F(CalendarQueueTest, ManyTimers) {
    CalendarQueue q;
    int fired = 0;
    for (int i = 0; i < 1000; i++) {
        auto id = q.schedule(ONE_MS, [&fired] { fired++; });
        EXPECT_GE(id, 1U);
    }
    EXPECT_EQ(q.size(), 1000U);
    q.advance(ONE_MS);     // init
    q.advance(3 * ONE_MS); // fire all
    EXPECT_EQ(fired, 1000);
    EXPECT_EQ(q.size(), 0U);
}

TEST_F(CalendarQueueTest, Empty) {
    CalendarQueue q;
    EXPECT_TRUE(q.empty());
    EXPECT_EQ(q.size(), 0U);

    auto id1 = q.schedule(ONE_MS, [] {});
    EXPECT_FALSE(q.empty());
    EXPECT_EQ(q.size(), 1U);

    auto id2 = q.schedule(ONE_MS, [] {});
    EXPECT_EQ(q.size(), 2U);

    q.cancel(id1);
    EXPECT_EQ(q.size(), 1U);

    q.advance(ONE_MS);
    q.advance(3 * ONE_MS);
    EXPECT_TRUE(q.empty());
    EXPECT_EQ(q.size(), 0U);

    // id2 was already cancelled by advance (it fired), so cancel should fail
    EXPECT_FALSE(q.cancel(id2));
}
