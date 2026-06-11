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
/// \brief Deterministic formal-validation tests for MPSCActorMailbox
///        thread-safety bugs.
///
/// All tests use scheduler_threads=0 and direct mailbox manipulation
/// to reproduce the exact interleaving traces from the formal analysis.
/// MockScheduler records notify_ready() calls without real threads.

#include <hpactor/mailbox/mpsc_actor_mailbox.hpp>
#include <hpactor/msg/enqueue_result.hpp>
#include <hpactor/msg/typed_message.hpp>
#include <hpactor/sched/scheduler.hpp>

#include <gtest/gtest.h>

#include <atomic>

using namespace hpactor;
using namespace hpactor::mailbox;

// ==========================================================================
// Mock scheduler — records notify_ready() calls without real threads.
// ==========================================================================
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

// ==========================================================================
// Bug 1 — DrainOverflowByteAccounting
// ==========================================================================
class DrainOverflowByteAccounting : public ::testing::Test {
  protected:
    void SetUp() override {
        cfg.capacity.max_messages = 4;
        cfg.capacity.max_bytes = 2048;
        cfg.overflow_policy = OverflowPolicy::SpillToOverflowQueue;
        cfg.max_overflow_depth = 8;
    }

    MailboxConfig cfg;
    MockScheduler scheduler;
};

TEST_F(DrainOverflowByteAccounting, NoUnderflowWhenDrainReservesCorrectBytes) {
    MPSCActorMailbox<TypedMessage> mb(ActorId{1}, &scheduler, cfg);

    // Fill the mailbox to capacity.
    for (int i = 0; i < 4; i++) {
        TypedMessage m;
        m.set_type_id(TypeTag::User);
        MailboxEnvelopeMeta meta;
        meta.type_tag = TypeTag::User;
        auto result = mb.try_push(std::move(m), meta);
        ASSERT_TRUE(result.accepted());
    }

    // Push one more — should spill to overflow.
    {
        TypedMessage m;
        m.set_type_id(TypeTag::User);
        MailboxEnvelopeMeta meta;
        meta.type_tag = TypeTag::User;
        auto result = mb.try_push(std::move(m), meta);
        EXPECT_EQ(result.code, EnqueueResultCode::ReroutedToOverflow);
    }

    uint64_t bytes_before = mb.snapshot().queued_bytes;

    // Drain all messages via dequeue (this triggers drain_overflow).
    for (int i = 0; i < 5; i++) {
        TypedMessage* node = mb.dequeue();
        ASSERT_NE(node, nullptr) << "expected message " << i;
        node->~TypedMessage();
        mem::deallocate(node);
    }

    // After draining all messages, queued_bytes_ must be 0, not ~2^64.
    uint64_t bytes_after = mb.snapshot().queued_bytes;
    EXPECT_EQ(bytes_after, 0u)
        << "queued_bytes_ underflowed! bytes_before=" << bytes_before;

    // Subsequent enqueues must still succeed.
    {
        TypedMessage m;
        m.set_type_id(TypeTag::User);
        MailboxEnvelopeMeta meta;
        meta.type_tag = TypeTag::User;
        auto result = mb.try_push(std::move(m), meta);
        EXPECT_TRUE(result.accepted())
            << "mailbox bricked after drain — byte capacity permanently exceeded";
    }
}

TEST_F(DrainOverflowByteAccounting, SubsequentEnqueuesSucceedAfterDrain) {
    cfg.capacity.max_messages = 1;
    MPSCActorMailbox<TypedMessage> mb(ActorId{2}, &scheduler, cfg);

    // Fill mailbox.
    {
        TypedMessage m;
        m.set_type_id(TypeTag::User);
        MailboxEnvelopeMeta meta;
        meta.type_tag = TypeTag::User;
        ASSERT_TRUE(mb.try_push(std::move(m), meta).accepted());
    }

    // Spill to overflow.
    for (int i = 0; i < 3; i++) {
        TypedMessage m;
        m.set_type_id(TypeTag::User);
        MailboxEnvelopeMeta meta;
        meta.type_tag = TypeTag::User;
        auto result = mb.try_push(std::move(m), meta);
        ASSERT_EQ(result.code, EnqueueResultCode::ReroutedToOverflow);
    }

    // Drain all messages (dequeue + drain_overflow cycles).
    for (int i = 0; i < 4; i++) {
        TypedMessage* node = mb.dequeue();
        ASSERT_NE(node, nullptr);
        node->~TypedMessage();
        mem::deallocate(node);
    }
    ASSERT_TRUE(mb.empty());

    // Enqueue 10 more messages — all must succeed.
    for (int i = 0; i < 10; i++) {
        TypedMessage m;
        m.set_type_id(TypeTag::User);
        MailboxEnvelopeMeta meta;
        meta.type_tag = TypeTag::User;
        auto result = mb.try_push(std::move(m), meta);
        EXPECT_TRUE(result.accepted())
            << "enqueue " << i << " failed after overflow drain cycle";
        // Drain immediately to make room.
        TypedMessage* node = mb.dequeue();
        ASSERT_NE(node, nullptr);
        node->~TypedMessage();
        mem::deallocate(node);
    }

    uint64_t final_bytes = mb.snapshot().queued_bytes;
    EXPECT_EQ(final_bytes, 0u) << "queued_bytes_ drifted after 10 drain cycles";
}

// ==========================================================================
// Bug 2 — WakeupProtocolRace
// ==========================================================================
class WakeupProtocolRace : public ::testing::Test {
  protected:
    void SetUp() override {
        cfg.capacity.max_messages = 8;
        scheduler.notify_ready_count.store(0);
    }

    MailboxConfig cfg;
    MockScheduler scheduler;
};

TEST_F(WakeupProtocolRace, ProducerWakeupAfterConsumerResetsFlag) {
    MPSCActorMailbox<TypedMessage> mb(ActorId{3}, &scheduler, cfg);

    // Inject one message (simulating a message already in the mailbox).
    {
        auto* raw = static_cast<TypedMessage*>(mem::allocate(
            mem::RegionType::kMessage, sizeof(TypedMessage), ActorId{3}));
        auto* node = new (raw) TypedMessage();
        node->set_type_id(TypeTag::User);
        mb.inject_for_test(node);
    }

    // Dequeue it — consumer resets mailbox_was_empty_ = true.
    {
        TypedMessage* node = mb.dequeue();
        ASSERT_NE(node, nullptr);
        node->~TypedMessage();
        mem::deallocate(node);
    }
    EXPECT_TRUE(mb.was_empty());

    // Now enqueue — must trigger wakeup since flag was true.
    int before = scheduler.notify_ready_count.load();
    {
        TypedMessage m;
        m.set_type_id(TypeTag::User);
        MailboxEnvelopeMeta meta;
        meta.type_tag = TypeTag::User;
        mb.push(std::move(m));
    }
    int after = scheduler.notify_ready_count.load();
    EXPECT_GT(after, before) << "wakeup was lost — notify_ready not called";
}

TEST_F(WakeupProtocolRace, NoSpuriousWakeupWhenMailboxAlreadyNonEmpty) {
    MPSCActorMailbox<TypedMessage> mb(ActorId{4}, &scheduler, cfg);

    // Inject two messages.
    for (int i = 0; i < 2; i++) {
        auto* raw = static_cast<TypedMessage*>(mem::allocate(
            mem::RegionType::kMessage, sizeof(TypedMessage), ActorId{4}));
        auto* node = new (raw) TypedMessage();
        node->set_type_id(TypeTag::User);
        mb.inject_for_test(node);
    }

    // Dequeue only one — mailbox still has one, flag is false.
    {
        TypedMessage* node = mb.dequeue();
        ASSERT_NE(node, nullptr);
        node->~TypedMessage();
        mem::deallocate(node);
    }
    EXPECT_FALSE(mb.empty());
    EXPECT_FALSE(mb.was_empty());

    // Enqueue — should NOT trigger another wakeup (actor already scheduled).
    int before = scheduler.notify_ready_count.load();
    {
        TypedMessage m;
        m.set_type_id(TypeTag::User);
        MailboxEnvelopeMeta meta;
        meta.type_tag = TypeTag::User;
        mb.push(std::move(m));
    }
    int after = scheduler.notify_ready_count.load();
    EXPECT_EQ(after, before)
        << "spurious wakeup — notify_ready called when mailbox was non-empty";
}

// ==========================================================================
// Bug 4 — SystemLaneDepthGuard
// ==========================================================================
class SystemLaneDepthGuard : public ::testing::Test {
  protected:
    void SetUp() override {
        cfg.capacity.max_messages = 16;
        cfg.protected_system_messages = 2;
    }

    MailboxConfig cfg;
    MockScheduler scheduler;
};

TEST_F(SystemLaneDepthGuard, RejectsWhenAtLimit) {
    MPSCActorMailbox<TypedMessage> mb(ActorId{5}, &scheduler, cfg);

    // Enqueue up to the limit.
    for (uint32_t i = 0; i < cfg.protected_system_messages; i++) {
        TypedMessage m;
        m.set_type_id(TypeTag::DownMsg);
        MailboxEnvelopeMeta meta;
        meta.type_tag = TypeTag::DownMsg;
        auto result = mb.try_push(std::move(m), meta);
        ASSERT_TRUE(result.accepted())
            << "system message " << i << " should be accepted (below limit)";
    }

    // Next system message must be rejected.
    {
        TypedMessage m;
        m.set_type_id(TypeTag::DownMsg);
        MailboxEnvelopeMeta meta;
        meta.type_tag = TypeTag::DownMsg;
        auto result = mb.try_push(std::move(m), meta);
        EXPECT_EQ(result.code, EnqueueResultCode::Rejected)
            << "system message should be rejected at limit";
    }
}

TEST_F(SystemLaneDepthGuard, ReleasesOnDequeue) {
    MPSCActorMailbox<TypedMessage> mb(ActorId{6}, &scheduler, cfg);

    // Enqueue one system message.
    {
        TypedMessage m;
        m.set_type_id(TypeTag::DownMsg);
        MailboxEnvelopeMeta meta;
        meta.type_tag = TypeTag::DownMsg;
        ASSERT_TRUE(mb.try_push(std::move(m), meta).accepted());
    }

    // Dequeue it.
    {
        TypedMessage* node = mb.dequeue();
        ASSERT_NE(node, nullptr);
        node->~TypedMessage();
        mem::deallocate(node);
    }

    // After dequeue, we should be able to enqueue up to the limit again.
    for (uint32_t i = 0; i < cfg.protected_system_messages; i++) {
        TypedMessage m;
        m.set_type_id(TypeTag::DownMsg);
        MailboxEnvelopeMeta meta;
        meta.type_tag = TypeTag::DownMsg;
        auto result = mb.try_push(std::move(m), meta);
        ASSERT_TRUE(result.accepted())
            << "system message " << i << " should be accepted after dequeue";
    }
}

// ==========================================================================
// Bug 5 — DroppedExistingRetryAccounting
// ==========================================================================
class DroppedExistingRetryAccounting : public ::testing::Test {
  protected:
    void SetUp() override {
        cfg.capacity.max_messages = 2;
        cfg.overflow_policy = OverflowPolicy::DropOldest;
    }

    MailboxConfig cfg;
    MockScheduler scheduler;
};

TEST_F(DroppedExistingRetryAccounting, RejectedCounterNotIncrementedOnRetrySuccess) {
    MPSCActorMailbox<TypedMessage> mb(ActorId{7}, &scheduler, cfg);

    // Fill mailbox to capacity.
    for (int i = 0; i < 2; i++) {
        TypedMessage m;
        m.set_type_id(TypeTag::User);
        MailboxEnvelopeMeta meta;
        meta.type_tag = TypeTag::User;
        ASSERT_TRUE(mb.try_push(std::move(m), meta).accepted());
    }

    auto snap_before = mb.snapshot();

    // The next enqueue triggers DropOldest overflow — drops oldest — retries.
    {
        TypedMessage m;
        m.set_type_id(TypeTag::User);
        MailboxEnvelopeMeta meta;
        meta.type_tag = TypeTag::User;
        auto result = mb.try_push(std::move(m), meta);
        EXPECT_TRUE(result.accepted());
    }

    auto snap_after = mb.snapshot();
    EXPECT_EQ(snap_after.total_dropped, snap_before.total_dropped + 1)
        << "expected one message dropped";
    EXPECT_EQ(snap_after.total_rejected, snap_before.total_rejected)
        << "total_rejected should not increase on successful DroppedExisting retry";
}
