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

#include <hpactor/actor/typed_message.hpp>
#include <hpactor/mailbox/mpsc_actor_mailbox.hpp>
#include <hpactor/sched/scheduler.hpp>

#include <atomic>
#include <gtest/gtest.h>
#include <thread>
#include <vector>

struct NoopScheduler : public hpactor::sched::IScheduler {
    void start() override {}
    void stop() override {}
    void notify_ready(hpactor::ActorId, uint8_t, int64_t) override {}
    void notify_idle(hpactor::ActorId) override {}
    void yield(hpactor::ActorId, uint8_t) override {}
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
};

using namespace hpactor;
using namespace hpactor::mailbox;

namespace {

constexpr size_t kProducers = 8;
constexpr size_t kMessagesPerProducer = 5000;
constexpr uint32_t kCapacity = 256;

} // namespace

TEST(MailboxBackpressureStressTest, RejectNewestStress) {
    NoopScheduler scheduler;

    MailboxConfig cfg;
    cfg.capacity.max_messages = kCapacity;
    cfg.overflow_policy = OverflowPolicy::RejectNewest;

    MPSCActorMailbox<TypedMessage> mailbox(ActorId{99}, &scheduler, cfg);

    std::atomic<bool> start{false};
    std::atomic<size_t> accepted_count{0};
    std::atomic<size_t> rejected_count{0};

    std::vector<std::thread> producers;
    for (size_t p = 0; p < kProducers; ++p) {
        producers.emplace_back([&, p]() {
            while (!start.load(std::memory_order_acquire)) {
            }
            for (size_t i = 0; i < kMessagesPerProducer; ++i) {
                auto msg = TypedMessage(
                    TypeTag::User, StreamBuffer{static_cast<uint8_t>(p),
                                                static_cast<uint8_t>(i & 0xFF)});
                auto result = mailbox.try_push(std::move(msg));
                if (result.accepted()) {
                    accepted_count.fetch_add(1, std::memory_order_relaxed);
                } else {
                    rejected_count.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    start.store(true, std::memory_order_release);
    for (auto& t : producers) {
        t.join();
    }

    size_t total_attempted = kProducers * kMessagesPerProducer;
    size_t accepted = accepted_count.load(std::memory_order_acquire);
    size_t rejected = rejected_count.load(std::memory_order_acquire);

    // All messages must be accounted for.
    EXPECT_EQ(accepted + rejected, total_attempted);

    // Some must have been rejected — proves bounded behaviour.
    EXPECT_GT(rejected, 0u);

    // Depth must never exceed hard capacity.
    auto snap = mailbox.snapshot();
    EXPECT_LE(snap.depth, kCapacity);
    EXPECT_LE(snap.max_depth, kCapacity);

    // Drain and verify.
    size_t drained = 0;
    TypedMessage out;
    while (mailbox.try_pop(out)) {
        drained++;
    }
    EXPECT_LE(drained, kCapacity);
    EXPECT_EQ(mailbox.snapshot().depth, 0u);
}

TEST(MailboxBackpressureStressTest, DropNewestStress) {
    NoopScheduler scheduler;

    MailboxConfig cfg;
    cfg.capacity.max_messages = kCapacity;
    cfg.overflow_policy = OverflowPolicy::DropNewest;

    MPSCActorMailbox<TypedMessage> mailbox(ActorId{100}, &scheduler, cfg);

    std::atomic<bool> start{false};
    std::atomic<size_t> accepted_count{0};
    std::atomic<size_t> dropped_count{0};
    std::atomic<size_t> rejected_count{0};

    std::vector<std::thread> producers;
    for (size_t p = 0; p < kProducers; ++p) {
        producers.emplace_back([&, p]() {
            while (!start.load(std::memory_order_acquire)) {
            }
            for (size_t i = 0; i < kMessagesPerProducer; ++i) {
                auto msg = TypedMessage(
                    TypeTag::User, StreamBuffer{static_cast<uint8_t>(p),
                                                static_cast<uint8_t>(i & 0xFF)});
                auto result = mailbox.try_push(std::move(msg));
                if (result.accepted()) {
                    accepted_count.fetch_add(1, std::memory_order_relaxed);
                } else if (result.code == EnqueueResultCode::DroppedNewest) {
                    dropped_count.fetch_add(1, std::memory_order_relaxed);
                } else {
                    rejected_count.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    start.store(true, std::memory_order_release);
    for (auto& t : producers) {
        t.join();
    }

    size_t total_attempted = kProducers * kMessagesPerProducer;
    size_t accepted = accepted_count.load(std::memory_order_acquire);
    size_t dropped = dropped_count.load(std::memory_order_acquire);
    size_t rejected = rejected_count.load(std::memory_order_acquire);

    // Full accounting.
    EXPECT_EQ(accepted + dropped + rejected, total_attempted);

    // Some failures must have occurred.
    EXPECT_GT(dropped + rejected, 0u);

    // Depth never exceeds capacity.
    auto snap = mailbox.snapshot();
    EXPECT_LE(snap.depth, kCapacity);
    EXPECT_LE(snap.max_depth, kCapacity);

    // Drain.
    size_t drained = 0;
    TypedMessage out;
    while (mailbox.try_pop(out)) {
        drained++;
    }
    EXPECT_LE(drained, kCapacity);
    EXPECT_EQ(mailbox.snapshot().depth, 0u);
}

TEST(MailboxBackpressureStressTest, DeadLetterStress) {
    NoopScheduler scheduler;

    MailboxConfig cfg;
    cfg.capacity.max_messages = kCapacity;
    cfg.overflow_policy = OverflowPolicy::DeadLetter;

    MPSCActorMailbox<TypedMessage> mailbox(ActorId{101}, &scheduler, cfg);

    std::atomic<bool> start{false};
    std::atomic<size_t> accepted_count{0};
    std::atomic<size_t> dead_letter_count{0};

    std::vector<std::thread> producers;
    for (size_t p = 0; p < kProducers; ++p) {
        producers.emplace_back([&, p]() {
            while (!start.load(std::memory_order_acquire)) {
            }
            for (size_t i = 0; i < kMessagesPerProducer; ++i) {
                auto msg = TypedMessage(
                    TypeTag::User, StreamBuffer{static_cast<uint8_t>(p),
                                                static_cast<uint8_t>(i & 0xFF)});
                auto result = mailbox.try_push(std::move(msg));
                if (result.accepted()) {
                    accepted_count.fetch_add(1, std::memory_order_relaxed);
                } else if (result.code == EnqueueResultCode::ReroutedToDeadLetter) {
                    dead_letter_count.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    start.store(true, std::memory_order_release);
    for (auto& t : producers) {
        t.join();
    }

    size_t total_attempted = kProducers * kMessagesPerProducer;
    size_t accepted = accepted_count.load(std::memory_order_acquire);
    size_t dead_letters = dead_letter_count.load(std::memory_order_acquire);

    // All messages must be accounted for.
    EXPECT_EQ(accepted + dead_letters, total_attempted);

    // Some dead-letter reroutes must have occurred.
    EXPECT_GT(dead_letters, 0u);

    // Depth never exceeds capacity.
    auto snap = mailbox.snapshot();
    EXPECT_LE(snap.depth, kCapacity);
    EXPECT_LE(snap.max_depth, kCapacity);

    // Drain.
    size_t drained = 0;
    TypedMessage out;
    while (mailbox.try_pop(out)) {
        drained++;
    }
    EXPECT_LE(drained, kCapacity);
}

TEST(MailboxBackpressureStressTest, SystemMessageReserveUnderStress) {
    NoopScheduler scheduler;

    MailboxConfig cfg;
    cfg.capacity.max_messages = kCapacity;
    cfg.overflow_policy = OverflowPolicy::RejectNewest;
    cfg.protected_system_messages = 8;

    MPSCActorMailbox<TypedMessage> mailbox(ActorId{102}, &scheduler, cfg);

    // Fill the mailbox with user messages to capacity.
    for (uint32_t i = 0; i < kCapacity; ++i) {
        auto msg =
            TypedMessage(TypeTag::User, StreamBuffer{static_cast<uint8_t>(i),
                                                     static_cast<uint8_t>(0)});
        auto result = mailbox.try_push(std::move(msg));
        ASSERT_TRUE(result.accepted());
    }

    // Next user message should be rejected.
    {
        auto msg =
            TypedMessage(TypeTag::User, StreamBuffer{static_cast<uint8_t>(0xFF)});
        auto result = mailbox.try_push(std::move(msg));
        EXPECT_FALSE(result.accepted());
    }

    // System message (DownMsg) should still be accepted via protected
    // reserve — must set type_tag in MailboxEnvelopeMeta so
    // is_system_message() check works in try_push.
    {
        auto msg = TypedMessage(TypeTag::DownMsg,
                                StreamBuffer{static_cast<uint8_t>(0x01)});
        MailboxEnvelopeMeta meta;
        meta.type_tag = TypeTag::DownMsg;
        meta.estimated_bytes = estimate_message_bytes(msg);
        auto result = mailbox.try_push(std::move(msg), meta);
        EXPECT_TRUE(result.accepted());
    }

    auto snap = mailbox.snapshot();
    EXPECT_EQ(snap.total_enqueued, kCapacity + 1); // capacity user + 1 system
    EXPECT_GE(snap.total_rejected, 1u); // at least the one user rejection
}
