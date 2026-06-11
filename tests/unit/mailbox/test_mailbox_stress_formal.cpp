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

/// \file
/// \brief Multi-threaded stress tests for MPSCActorMailbox thread-safety.
///
/// Designed to run under TSAN and ASAN.

#include <hpactor/mailbox/mpsc_actor_mailbox.hpp>
#include <hpactor/msg/enqueue_result.hpp>
#include <hpactor/msg/typed_message.hpp>
#include <hpactor/sched/scheduler.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <random>
#include <thread>
#include <vector>

using namespace hpactor;
using namespace hpactor::mailbox;

struct StressMockScheduler : public hpactor::sched::IScheduler {
    void start() override {}
    void stop() override {}
    void notify_ready(hpactor::ActorId, uint8_t, int64_t) override {
        notify_count.fetch_add(1, std::memory_order_relaxed);
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

    std::atomic<uint64_t> notify_count{0};
};

// ==========================================================================
// Bug 1 + 2 + 4 — ConcurrentProducerFlood
// ==========================================================================
class ConcurrentProducerFlood : public ::testing::Test {
  protected:
    void SetUp() override {
        cfg.capacity.max_messages = 64;
        cfg.capacity.max_bytes = 65536;
        cfg.overflow_policy = OverflowPolicy::DropOldest;
        cfg.protected_system_messages = 4;
    }

    MailboxConfig cfg;
    StressMockScheduler scheduler;
};

TEST_F(ConcurrentProducerFlood, ByteAccountingNeverUnderflows) {
    MPSCActorMailbox<TypedMessage> mb(ActorId{100}, &scheduler, cfg);

    constexpr int kProducers = 8;
    constexpr int kMsgsPerProducer = 10000;
    std::atomic<bool> start{false};
    std::atomic<int> total_sent{0};
    std::vector<std::thread> producers;

    for (int t = 0; t < kProducers; t++) {
        producers.emplace_back([&]() {
            while (!start.load(std::memory_order_acquire)) {
            }
            for (int i = 0; i < kMsgsPerProducer; i++) {
                TypedMessage m;
                m.set_type_id(TypeTag::User);
                MailboxEnvelopeMeta meta;
                meta.type_tag = TypeTag::User;
                meta.priority = static_cast<uint8_t>(i % 4);
                if (i % 10 == 0) {
                    m.set_type_id(TypeTag::DownMsg);
                    meta.type_tag = TypeTag::DownMsg;
                }
                mb.push(std::move(m));
                total_sent.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    std::atomic<uint64_t> total_dequeued{0};
    std::thread consumer([&]() {
        while (!start.load(std::memory_order_acquire)) {
        }
        int idle_spins = 0;
        while (total_dequeued.load(std::memory_order_relaxed) <
                   kProducers * kMsgsPerProducer &&
               idle_spins < 1000) {
            TypedMessage* node = mb.dequeue();
            if (node) {
                node->~TypedMessage();
                mem::deallocate(node);
                total_dequeued.fetch_add(1, std::memory_order_relaxed);
                idle_spins = 0;
            } else {
                idle_spins++;
                std::this_thread::yield();
            }
        }
    });

    start.store(true, std::memory_order_release);

    for (auto& t : producers) {
        t.join();
    }
    consumer.join();

    auto snap = mb.snapshot();
    uint64_t accounted = snap.total_enqueued + snap.total_rejected +
                         snap.total_dropped + snap.total_dead_letters;
    EXPECT_EQ(accounted, static_cast<uint64_t>(total_sent.load()))
        << "message accounting mismatch";

    EXPECT_LT(snap.queued_bytes, uint64_t(1) << 63)
        << "queued_bytes_ underflowed (wrapped to > 2^63)";
}

TEST_F(ConcurrentProducerFlood, AllMessagesAccountedFor) {
    MPSCActorMailbox<TypedMessage> mb(ActorId{101}, &scheduler, cfg);

    constexpr int kProducers = 4;
    constexpr int kMsgsPerProducer = 5000;
    std::atomic<bool> start{false};
    std::atomic<int> total_sent{0};
    std::vector<std::thread> producers;

    for (int t = 0; t < kProducers; t++) {
        producers.emplace_back([&]() {
            while (!start.load(std::memory_order_acquire)) {
            }
            for (int i = 0; i < kMsgsPerProducer; i++) {
                TypedMessage m;
                m.set_type_id(TypeTag::User);
                MailboxEnvelopeMeta meta;
                meta.type_tag = TypeTag::User;
                mb.push(std::move(m));
                total_sent.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    start.store(true, std::memory_order_release);
    for (auto& t : producers) {
        t.join();
    }

    int drained = 0;
    int idle = 0;
    while (idle < 100) {
        TypedMessage* node = mb.dequeue();
        if (node) {
            node->~TypedMessage();
            mem::deallocate(node);
            drained++;
            idle = 0;
        } else {
            idle++;
        }
    }

    auto snap = mb.snapshot();
    uint64_t accounted = snap.total_enqueued + snap.total_rejected +
                         snap.total_dropped + snap.total_dead_letters;
    EXPECT_EQ(accounted, static_cast<uint64_t>(total_sent.load()))
        << "not all messages accounted for: sent=" << total_sent.load()
        << " accounted=" << accounted;
}

// ==========================================================================
// Bug 2 — LostWakeupDetection
// ==========================================================================
class LostWakeupDetection : public ::testing::Test {
  protected:
    void SetUp() override {
        cfg.capacity.max_messages = 256;
    }

    MailboxConfig cfg;
    StressMockScheduler scheduler;
};

TEST_F(LostWakeupDetection, NoMessagesLeftBehindSingleProducer) {
    MPSCActorMailbox<TypedMessage> mb(ActorId{200}, &scheduler, cfg);

    constexpr int kTotal = 1000;
    std::atomic<bool> producer_done{false};
    std::atomic<int> dequeued{0};
    std::mt19937 rng(42);

    std::thread producer([&]() {
        for (int i = 0; i < kTotal; i++) {
            TypedMessage m;
            m.set_type_id(TypeTag::User);
            MailboxEnvelopeMeta meta;
            meta.type_tag = TypeTag::User;
            mb.push(std::move(m));
            if (i % 10 == 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(rng() % 50));
            }
        }
        producer_done.store(true, std::memory_order_release);
    });

    std::thread consumer([&]() {
        while (!producer_done.load(std::memory_order_acquire) ||
               dequeued.load(std::memory_order_relaxed) < kTotal) {
            TypedMessage* node = mb.dequeue();
            if (node) {
                node->~TypedMessage();
                mem::deallocate(node);
                dequeued.fetch_add(1, std::memory_order_relaxed);
            }
            if (dequeued.load() % 7 == 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(rng() % 5000));
            }
        }
    });

    producer.join();
    consumer.join();

    EXPECT_EQ(dequeued.load(), kTotal)
        << "lost messages: sent " << kTotal << " but only dequeued "
        << dequeued.load();
}

TEST_F(LostWakeupDetection, NoMessagesLeftBehindMultiProducer) {
    MPSCActorMailbox<TypedMessage> mb(ActorId{201}, &scheduler, cfg);

    constexpr int kProducers = 16;
    constexpr int kMsgsPerProducer = 500;
    std::atomic<bool> start{false};
    std::atomic<int> total_dequeued{0};
    std::mt19937 rng(99);

    std::vector<std::thread> producers;
    for (int t = 0; t < kProducers; t++) {
        producers.emplace_back([&, t]() {
            while (!start.load(std::memory_order_acquire)) {
            }
            std::mt19937 local_rng(static_cast<unsigned>(t * 137 + 42));
            for (int i = 0; i < kMsgsPerProducer; i++) {
                TypedMessage m;
                m.set_type_id(TypeTag::User);
                MailboxEnvelopeMeta meta;
                meta.type_tag = TypeTag::User;
                mb.push(std::move(m));
                if (i % 20 == 0) {
                    std::this_thread::sleep_for(
                        std::chrono::microseconds(local_rng() % 100));
                }
            }
        });
    }

    std::thread consumer([&]() {
        while (!start.load(std::memory_order_acquire)) {
        }
        int idle = 0;
        int expected = kProducers * kMsgsPerProducer;
        while (total_dequeued.load(std::memory_order_relaxed) < expected &&
               idle < 5000) {
            TypedMessage* node = mb.dequeue();
            if (node) {
                node->~TypedMessage();
                mem::deallocate(node);
                total_dequeued.fetch_add(1, std::memory_order_relaxed);
                idle = 0;
            } else {
                idle++;
                std::this_thread::yield();
            }
            if (total_dequeued.load() % 13 == 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(rng() % 3000));
            }
        }
    });

    start.store(true, std::memory_order_release);

    for (auto& t : producers) {
        t.join();
    }
    consumer.join();

    EXPECT_EQ(total_dequeued.load(), kProducers * kMsgsPerProducer)
        << "lost messages in multi-producer test";
}

// ==========================================================================
// Bug 3 — PendingFreeConcurrency
// ==========================================================================
class PendingFreeConcurrency : public ::testing::Test {
  protected:
    void SetUp() override {
        cfg.capacity.max_messages = 4;
        cfg.overflow_policy = OverflowPolicy::DropOldest;
    }

    MailboxConfig cfg;
    StressMockScheduler scheduler;
};

TEST_F(PendingFreeConcurrency, NoDoubleFreeUnderContention) {
    MPSCActorMailbox<TypedMessage> mb(ActorId{300}, &scheduler, cfg);

    constexpr int kProducers = 8;
    constexpr int kMsgsPerProducer = 5000;
    std::atomic<bool> start{false};
    std::atomic<int> producers_done{0};
    std::vector<std::thread> producers;

    for (int t = 0; t < kProducers; t++) {
        producers.emplace_back([&]() {
            while (!start.load(std::memory_order_acquire)) {
            }
            for (int i = 0; i < kMsgsPerProducer; i++) {
                TypedMessage m;
                m.set_type_id(TypeTag::User);
                MailboxEnvelopeMeta meta;
                meta.type_tag = TypeTag::User;
                mb.push(std::move(m));
            }
            producers_done.fetch_add(1, std::memory_order_relaxed);
        });
    }

    start.store(true, std::memory_order_release);

    std::atomic<int> total_dequeued{0};
    std::thread consumer([&]() {
        int idle = 0;
        while (producers_done.load(std::memory_order_relaxed) < kProducers ||
               idle < 200) {
            TypedMessage* node = mb.dequeue();
            if (node) {
                node->~TypedMessage();
                mem::deallocate(node);
                total_dequeued.fetch_add(1, std::memory_order_relaxed);
                idle = 0;
            } else {
                idle++;
                std::this_thread::yield();
            }
        }
    });

    for (auto& t : producers) {
        t.join();
    }
    consumer.join();

    while (true) {
        TypedMessage* node = mb.dequeue();
        if (!node) {
            break;
        }
        node->~TypedMessage();
        mem::deallocate(node);
    }

    SUCCEED() << "no crash under concurrent overflow eviction";
}
