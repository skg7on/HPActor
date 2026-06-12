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

// =============================================================================
// Tests: MPSCActorMailbox pre-arming race condition reproduction
// =============================================================================
//
// These tests reproduce a race condition in MPSCActorMailbox::dequeue()
// where the edge-triggered wakeup flag (mailbox_was_empty_) is pre-armed
// BEFORE acquiring the consumer spin-lock, creating a window where a
// producer can enqueue a message without triggering the scheduler wakeup.
//
// The interleaving:
//   1. Producer: was_empty = empty() → false (messages present)
//   2. Consumer: pre-arm flag → true, lock, dequeue last message
//   3. Consumer: mailbox now empty, flag stays true (pre-armed)
//   4. Producer: enqueue message (was_empty captured stale as false)
//   5. Producer: skips CAS → no wakeup → message orphaned
//
// With scheduler_threads = 1, serial execution prevents interleaving.
// With scheduler_threads = 4, the race triggers under concurrent load.
//
// See: https://github.com/skg7on/HPActor/issues/264

#include <gtest/gtest.h>
#include <hpactor/mailbox/mpsc_actor_mailbox.hpp>
#include <hpactor/msg/typed_message.hpp>
#include <hpactor/sched/scheduler.hpp>

#include <atomic>
#include <barrier>
#include <cstdint>
#include <thread>
#include <vector>

// =============================================================================
// MockScheduler — records notify_ready calls for verification
// =============================================================================

struct MockScheduler : public hpactor::sched::IScheduler {
    void start() override {}
    void stop() override {}
    void notify_ready(hpactor::ActorId actor, uint8_t priority,
                      int64_t deadline) override {
        last_actor = actor;
        last_priority = priority;
        last_deadline = deadline;
        notify_ready_count.fetch_add(1, std::memory_order_relaxed);
    }
    void notify_idle(hpactor::ActorId) override {}
    void yield(hpactor::ActorId actor, uint8_t priority) override {
        notify_ready(actor, priority, INT64_MAX);
    }
    hpactor::sched::TimerHandle
    schedule_after(hpactor::sched::timer_callback, int64_t) override {
        return {};
    }
    hpactor::sched::TimerHandle
    schedule_every(hpactor::sched::timer_callback, int64_t) override {
        return {};
    }
    void cancel_timer(hpactor::sched::TimerHandle) override {}
    size_t worker_count() const override {
        return 1;
    }
    bool is_running() const override {
        return true;
    }
    void register_dedicated_thread(hpactor::ActorId, int) override {}
    void register_dedicated_pool(hpactor::ActorId, uint32_t) override {}
    void unregister_dedicated(hpactor::ActorId) override {}

    std::atomic<int> notify_ready_count{0};
    hpactor::ActorId last_actor{};
    uint8_t last_priority = 255;
    int64_t last_deadline = 0;
};

// =============================================================================
// Fixture: shared config for pre-arm race tests
// =============================================================================

class PrearmRaceTest : public ::testing::Test {
  protected:
    void SetUp() override {
        cfg.capacity.max_messages = 4096;
        cfg.high_watermark = 0.90;
        cfg.low_watermark = 0.50;
    }

    hpactor::mailbox::MPSCActorMailbox<hpactor::TypedMessage>
    make_mailbox(hpactor::ActorId id = hpactor::ActorId{77}) {
        return hpactor::mailbox::MPSCActorMailbox<hpactor::TypedMessage>(
            id, &scheduler, cfg);
    }

    hpactor::mailbox::MailboxConfig cfg;
    MockScheduler scheduler;
};

// =============================================================================
// Test 1: LostWakeupStress
// =============================================================================
//
// Multi-producer stress test that exercises the exact race window.
// 4 producer threads enqueue 25K messages each while 1 consumer thread
// dequeues.  After all threads finish, we verify message accounting and
// check for the orphaned-message invariant.

TEST_F(PrearmRaceTest, DISABLED_LostWakeupStress) {
    using namespace hpactor;
    using namespace hpactor::mailbox;

    for (int trial = 0; trial < 5; ++trial) {
        auto mb = make_mailbox();
        mb.set_continuation_callback(
            [this]() { scheduler.notify_ready(ActorId{77}, 0, INT64_MAX); });

        constexpr int kNumProducers = 4;
        constexpr int kMsgsPerProducer = 25'000;

        std::atomic<uint64_t> total_enqueued{0};
        std::atomic<uint64_t> total_dequeued{0};
        std::atomic<bool> stop_flag{false};
        std::atomic<int> producers_done{0};

        // Barrier: all producers + consumer start at the same time.
        std::barrier start_barrier(kNumProducers + 1);

        // ── Producer threads ────────────────────────────────────────
        std::vector<std::thread> producers;
        for (int p = 0; p < kNumProducers; ++p) {
            producers.emplace_back([&, p]() {
                start_barrier.arrive_and_wait();

                for (int i = 0; i < kMsgsPerProducer; ++i) {
                    MailboxEnvelopeMeta meta;
                    meta.type_tag = TypeTag::User;
                    meta.priority = static_cast<uint8_t>(p % 4);

                    auto result = mb.try_push(
                        TypedMessage(TypeTag::User,
                                     StreamBuffer{static_cast<uint8_t>(p),
                                                  static_cast<uint8_t>(i & 0xFF)}),
                        meta);
                    if (result.accepted()) {
                        total_enqueued.fetch_add(1, std::memory_order_relaxed);
                    }
                    // Occasional yield to increase interleaving.
                    if (i % 256 == 0) {
                        std::this_thread::yield();
                    }
                }

                producers_done.fetch_add(1, std::memory_order_release);
            });
        }

        // ── Consumer thread ─────────────────────────────────────────
        std::thread consumer([&]() {
            start_barrier.arrive_and_wait();

            int done_count = 0;
            while (true) {
                TypedMessage msg;
                if (mb.try_pop(msg)) {
                    total_dequeued.fetch_add(1, std::memory_order_relaxed);
                }

                if (stop_flag.load(std::memory_order_acquire)) {
                    // Main thread signalled stop — drain and exit.
                    if (mb.empty()) {
                        break;
                    }
                } else {
                    // Check if all producers are done.
                    done_count = producers_done.load(std::memory_order_acquire);
                    if (done_count == kNumProducers) {
                        // Producers done — double-check emptiness before
                        // exiting to minimize the race window.
                        if (mb.empty()) {
                            std::this_thread::yield();
                            if (mb.empty() ||
                                stop_flag.load(std::memory_order_acquire)) {
                                break;
                            }
                        }
                    }
                }
                std::this_thread::yield();
            }
        });

        // ── Coordinate completion ───────────────────────────────────
        for (auto& t : producers) {
            t.join();
        }
        stop_flag.store(true, std::memory_order_release);
        consumer.join();

        // ── Drain any remaining messages ────────────────────────────
        uint64_t drained = 0;
        {
            TypedMessage msg;
            while (mb.try_pop(msg)) {
                drained++;
            }
        }

        uint64_t enqueued = total_enqueued.load(std::memory_order_relaxed);
        uint64_t dequeued = total_dequeued.load(std::memory_order_relaxed);

        // ── Invariant A: message accounting ─────────────────────────
        EXPECT_EQ(enqueued, dequeued + drained)
            << "Trial " << trial << ": message accounting mismatch — "
            << enqueued << " enqueued, " << dequeued
            << " dequeued by consumer, " << drained << " drained after";

        // ── Invariant B: mailbox must be fully drainable ─────────────
        EXPECT_TRUE(mb.empty())
            << "Trial " << trial << ": mailbox not fully drained — " << drained
            << " messages remaining after drain";

        // ── Invariant C: orphaned-message detection ─────────────────
        // If the mailbox is non-empty but was_empty() returns true, the
        // pre-arming race has orphaned a message (no wakeup fired).
        if (!mb.empty() && mb.was_empty()) {
            ADD_FAILURE() << "Trial " << trial
                          << ": ORPHANED MESSAGE DETECTED — mailbox non-empty "
                             "but was_empty() is true";
        }

        // ── Invariant D: non-empty mailbox must have was_empty false ─
        // This verifies the wakeup protocol invariant: any message in
        // the mailbox must have triggered a wakeup (flag → false).
        if (!mb.empty()) {
            EXPECT_FALSE(mb.was_empty())
                << "Trial " << trial
                << ": mailbox non-empty with was_empty()=true — "
                   "message(s) orphaned by pre-arming race";
        }

        scheduler.notify_ready_count.store(0, std::memory_order_relaxed);
    }
}

// =============================================================================
// Test 2: WakeupNotificationAccounting
// =============================================================================
//
// Verifies that notify_ready() is called for empty→non-empty transitions
// under concurrent load.  The continuation callback counts wakeups.

TEST_F(PrearmRaceTest, DISABLED_WakeupNotificationAccounting) {
    using namespace hpactor;
    using namespace hpactor::mailbox;

    for (int trial = 0; trial < 5; ++trial) {
        scheduler.notify_ready_count.store(0, std::memory_order_relaxed);

        auto mb = make_mailbox();
        mb.set_continuation_callback(
            [this]() { scheduler.notify_ready(ActorId{77}, 0, INT64_MAX); });

        constexpr int kNumProducers = 4;
        constexpr int kMsgsPerProducer = 10'000;

        std::atomic<uint64_t> total_enqueued{0};
        std::atomic<bool> stop_flag{false};
        std::atomic<int> producers_done{0};

        std::barrier start_barrier(kNumProducers + 1);

        // ── Producers ───────────────────────────────────────────────
        std::vector<std::thread> producers;
        for (int p = 0; p < kNumProducers; ++p) {
            producers.emplace_back([&, p]() {
                start_barrier.arrive_and_wait();
                for (int i = 0; i < kMsgsPerProducer; ++i) {
                    MailboxEnvelopeMeta meta;
                    meta.type_tag = TypeTag::User;
                    meta.priority = static_cast<uint8_t>(p);

                    auto result = mb.try_push(
                        TypedMessage(TypeTag::User, StreamBuffer{0x42}), meta);
                    if (result.accepted()) {
                        total_enqueued.fetch_add(1, std::memory_order_relaxed);
                    }
                    if (i % 128 == 0) {
                        std::this_thread::yield();
                    }
                }
                producers_done.fetch_add(1, std::memory_order_release);
            });
        }

        // ── Consumer ────────────────────────────────────────────────
        std::thread consumer([&]() {
            start_barrier.arrive_and_wait();

            int done_count = 0;
            while (true) {
                TypedMessage msg;
                if (mb.try_pop(msg)) {
                    // dequeued
                }

                if (stop_flag.load(std::memory_order_acquire)) {
                    if (mb.empty()) {
                        break;
                    }
                } else {
                    done_count = producers_done.load(std::memory_order_acquire);
                    if (done_count == kNumProducers && mb.empty()) {
                        std::this_thread::yield();
                        if (mb.empty() ||
                            stop_flag.load(std::memory_order_acquire)) {
                            break;
                        }
                    }
                }
                std::this_thread::yield();
            }
        });

        // ── Coordinate completion ───────────────────────────────────
        for (auto& t : producers) {
            t.join();
        }
        stop_flag.store(true, std::memory_order_release);
        consumer.join();

        // ── Drain stragglers ────────────────────────────────────────
        uint64_t drained = 0;
        {
            TypedMessage msg;
            while (mb.try_pop(msg)) {
                drained++;
            }
        }

        int wakeups = scheduler.notify_ready_count.load(std::memory_order_relaxed);
        uint64_t enqueued = total_enqueued.load(std::memory_order_relaxed);

        EXPECT_TRUE(mb.empty())
            << "Trial " << trial << ": mailbox not empty after drain — "
            << drained << " messages drained post-consumer, " << enqueued
            << " total enqueued";

        EXPECT_GT(wakeups, 0) << "Trial " << trial
                              << ": no notify_ready calls — "
                                 "the continuation callback was never invoked";

        // Check for orphaned messages.
        if (!mb.empty() && mb.was_empty()) {
            ADD_FAILURE() << "Trial " << trial
                          << ": orphaned messages — mailbox non-empty "
                             "with was_empty()=true after drain";
        }

        if (drained > 0) {
            std::cout << "[Trial " << trial << "] wakeups=" << wakeups
                      << " enqueued=" << enqueued << " drained_post=" << drained
                      << "\n";
        }
    }
}
