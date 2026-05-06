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

// tests/sched/test_calendar_queue.cpp

#include <hpactor/sched/calendar_queue.hpp>
#include <hpactor/sched/scheduler.hpp>

#include <cassert>
#include <cstdio>
#include <functional>

using namespace hpactor::sched;

static constexpr int64_t ONE_MS = 1'000'000;

// ---------------------------------------------------------------------------
// test_basic_schedule
// ---------------------------------------------------------------------------
void test_basic_schedule() {
    CalendarQueue q;
    int fired = 0;
    auto id = q.schedule(ONE_MS, [&fired] { fired++; });
    assert(id >= 1);
    uint32_t r1 = q.advance(ONE_MS);       // init: sets clock origin
    assert(r1 == 0);
    uint32_t r2 = q.advance(3 * ONE_MS);   // advance through bucket containing timer
    assert(r2 == 1);
    assert(fired == 1);
    assert(q.size() == 0);
    printf("  PASSED test_basic_schedule\n");
}

// ---------------------------------------------------------------------------
// test_cancel
// ---------------------------------------------------------------------------
void test_cancel() {
    CalendarQueue q;
    int fired = 0;
    auto id = q.schedule(ONE_MS, [&fired] { fired++; });
    bool ok = q.cancel(id);
    assert(ok);
    assert(q.size() == 0);
    q.advance(ONE_MS);
    uint32_t r = q.advance(3 * ONE_MS);
    assert(r == 0);
    assert(fired == 0);
    printf("  PASSED test_cancel\n");
}

// ---------------------------------------------------------------------------
// test_cancel_nonexistent
// ---------------------------------------------------------------------------
void test_cancel_nonexistent() {
    CalendarQueue q;
    assert(!q.cancel(999));
    assert(!q.cancel(0));
    printf("  PASSED test_cancel_nonexistent\n");
}

// ---------------------------------------------------------------------------
// test_id_valid
// ---------------------------------------------------------------------------
void test_id_valid() {
    CalendarQueue q;
    auto id = q.schedule(ONE_MS, [] {});
    assert(id >= 1);
    TimerHandle h0{0};
    assert(!h0.valid());
    TimerHandle h1{id};
    assert(h1.valid());
    printf("  PASSED test_id_valid\n");
}

// ---------------------------------------------------------------------------
// test_zero_delay_clamped
// ---------------------------------------------------------------------------
void test_zero_delay_clamped() {
    CalendarQueue q;
    int fired = 0;
    auto id = q.schedule(0, [&fired] { fired++; });   // zero delay -> clamped to 1 fine bucket
    assert(id >= 1);
    q.advance(ONE_MS);
    uint32_t r = q.advance(3 * ONE_MS);
    assert(r == 1);
    assert(fired == 1);
    printf("  PASSED test_zero_delay_clamped\n");
}

// ---------------------------------------------------------------------------
// test_fine_coarse_split
// Schedule a timer whose delay exceeds the fine wheel range so it lands in
// the coarse wheel.  Use a small config so cascading is practical.
// ---------------------------------------------------------------------------
void test_fine_coarse_split() {
    // fine = 8 * 1 ms = 8 ms,  coarse = 8 * 8 ms = 64 ms
    CalendarQueueConfig cfg{ONE_MS, 8, 8, 8, 4096};
    CalendarQueue q(cfg);
    int fired = 0;

    // 10 ms delay  ->  10 > 8 (fine) and 10 < 64 (coarse)  ->  coarse wheel
    auto id = q.schedule(10 * ONE_MS, [&fired] { fired++; });
    assert(id >= 1);

    // Advance in 1 ms steps.  The timer cascades from coarse -> fine when
    // the fine wheel wraps enough times to reach the correct coarse bucket.
    for (int64_t t = ONE_MS; t <= 30 * ONE_MS; t += ONE_MS) {
        q.advance(t);
        if (fired > 0) break;
    }
    assert(fired == 1);
    printf("  PASSED test_fine_coarse_split\n");
}

// ---------------------------------------------------------------------------
// test_cascade_coarse
// 5 timers all land in the same coarse bucket; all must fire after cascading.
// ---------------------------------------------------------------------------
void test_cascade_coarse() {
    CalendarQueueConfig cfg{ONE_MS, 8, 8, 8, 4096};
    CalendarQueue q(cfg);
    int fired = 0;

    for (int i = 0; i < 5; i++) {
        auto id = q.schedule(10 * ONE_MS, [&fired] { fired++; });
        assert(id >= 1);
    }
    assert(q.size() == 5);

    for (int64_t t = ONE_MS; t <= 30 * ONE_MS; t += ONE_MS) {
        q.advance(t);
        if (fired == 5) break;
    }
    assert(fired == 5);
    assert(q.size() == 0);
    printf("  PASSED test_cascade_coarse\n");
}

// ---------------------------------------------------------------------------
// test_remote_cascade
// Timer lands in the remote wheel; verify it cascades through coarse and fine
// and eventually fires.  Use a compact config so the test is fast.
// ---------------------------------------------------------------------------
void test_remote_cascade() {
    // fine = 4 * 1 ms = 4 ms, coarse = 4 * 4 ms = 16 ms, remote = 4 * 16 ms = 64 ms
    CalendarQueueConfig cfg{ONE_MS, 4, 4, 4, 4096};
    CalendarQueue q(cfg);
    int fired = 0;

    // 30 ms  ->  30 > 16 (coarse) and 30 < 64 (fine+coarse range overridden by
    // remote check in insert_timer)  ->  remote wheel
    auto id = q.schedule(30 * ONE_MS, [&fired] { fired++; });
    assert(id >= 1);

    // Advance in 1 ms steps through enough time for the timer to cascade
    // from remote -> coarse -> fine and fire.
    for (int64_t t = ONE_MS; t <= 60 * ONE_MS; t += ONE_MS) {
        q.advance(t);
        if (fired > 0) break;
    }
    assert(fired == 1);
    printf("  PASSED test_remote_cascade\n");
}

// ---------------------------------------------------------------------------
// test_recurring_no_deadlock
// The callback re-schedules itself during advance() -- exercises the
// recursive_mutex (must not deadlock).
// ---------------------------------------------------------------------------
void test_recurring_no_deadlock() {
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

    q.advance(1);                     // init clock
    auto id = q.schedule(ONE_MS, cb);
    assert(id >= 1);

    int64_t t = 2 * ONE_MS;
    while (count < 5) {
        q.advance(t);
        t += ONE_MS;
    }
    assert(count == 5);
    printf("  PASSED test_recurring_no_deadlock\n");
}

// ---------------------------------------------------------------------------
// test_cancel_during_advance
// Callback A cancels timer B while A is being processed during advance().
// ---------------------------------------------------------------------------
void test_cancel_during_advance() {
    CalendarQueue q;
    int fired_a = 0;
    int fired_b = 0;
    uint64_t id_b = 0;

    auto cb_a = [&]() {
        fired_a++;
        q.cancel(id_b);
    };
    auto cb_b = [&]() {
        fired_b++;
    };

    auto id_a = q.schedule(ONE_MS, cb_a);         // A at bucket 1
    assert(id_a >= 1);
    id_b = q.schedule(2 * ONE_MS, cb_b);  // B at bucket 2
    assert(id_b >= 1);
    q.advance(ONE_MS);                // init
    uint32_t r = q.advance(3 * ONE_MS);
    assert(r == 1);
    assert(fired_a == 1);
    assert(fired_b == 0);
    printf("  PASSED test_cancel_during_advance\n");
}

// ---------------------------------------------------------------------------
// test_time_jump
// Multiple timers at different times; a single large advance fires all of them.
// ---------------------------------------------------------------------------
void test_time_jump() {
    CalendarQueue q;
    int fired = 0;
    auto id1 = q.schedule(1 * ONE_MS, [&fired] { fired++; });
    auto id2 = q.schedule(2 * ONE_MS, [&fired] { fired++; });
    auto id3 = q.schedule(3 * ONE_MS, [&fired] { fired++; });
    assert(id1 >= 1 && id2 >= 1 && id3 >= 1);
    q.advance(ONE_MS);                     // init
    uint32_t r = q.advance(5 * ONE_MS);     // should fire all 3
    assert(r == 3);
    assert(fired == 3);
    printf("  PASSED test_time_jump\n");
}

// ---------------------------------------------------------------------------
// test_time_backwards
// Advancing with a time earlier than last_advance returns 0.
// ---------------------------------------------------------------------------
void test_time_backwards() {
    CalendarQueue q;
    q.advance(5 * ONE_MS);     // init, last_advance = 5 ms
    q.advance(6 * ONE_MS);     // advance normally
    uint32_t r = q.advance(3 * ONE_MS);  // backwards -- must return 0
    assert(r == 0);
    printf("  PASSED test_time_backwards\n");
}

// ---------------------------------------------------------------------------
// test_many_timers
// Schedule 1000 timers; advance far enough to fire them all.
// ---------------------------------------------------------------------------
void test_many_timers() {
    CalendarQueue q;
    int fired = 0;
    for (int i = 0; i < 1000; i++) {
        auto id = q.schedule(ONE_MS, [&fired] { fired++; });
        assert(id >= 1);
    }
    assert(q.size() == 1000);
    q.advance(ONE_MS);                      // init
    q.advance(3 * ONE_MS);                  // fire all
    assert(fired == 1000);
    assert(q.size() == 0);
    printf("  PASSED test_many_timers\n");
}

// ---------------------------------------------------------------------------
// test_empty
// Verify empty() and size() across insert, cancel, and advance.
// ---------------------------------------------------------------------------
void test_empty() {
    CalendarQueue q;
    assert(q.empty());
    assert(q.size() == 0);

    auto id1 = q.schedule(ONE_MS, [] {});
    assert(!q.empty());
    assert(q.size() == 1);

    auto id2 = q.schedule(ONE_MS, [] {});
    assert(q.size() == 2);

    q.cancel(id1);
    assert(q.size() == 1);

    q.advance(ONE_MS);
    q.advance(3 * ONE_MS);
    assert(q.empty());
    assert(q.size() == 0);

    // id2 was already cancelled by advance (it fired), so cancel should fail
    assert(!q.cancel(id2));

    printf("  PASSED test_empty\n");
}

// ---------------------------------------------------------------------------
int main() {
    printf("CalendarQueue tests:\n");
    test_basic_schedule();
    test_cancel();
    test_cancel_nonexistent();
    test_id_valid();
    test_zero_delay_clamped();
    test_fine_coarse_split();
    test_cascade_coarse();
    test_remote_cascade();
    test_recurring_no_deadlock();
    test_cancel_during_advance();
    test_time_jump();
    test_time_backwards();
    test_many_timers();
    test_empty();
    printf("All CalendarQueue tests PASSED\n");
    return 0;
}
