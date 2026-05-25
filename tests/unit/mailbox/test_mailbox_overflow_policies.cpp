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
#include <hpactor/actor/typed_message.hpp>
#include <hpactor/mailbox/mpsc_actor_mailbox.hpp>
#include <hpactor/sched/scheduler.hpp>

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

class OverflowPolicyTest : public ::testing::Test {
  protected:
    NoopScheduler scheduler;
};

TEST_F(OverflowPolicyTest, DropNewestRejectsSecondMessageAtCapacity) {
    using namespace hpactor;
    using namespace hpactor::mailbox;

    MailboxConfig drop_newest;
    drop_newest.capacity.max_messages = 1;
    drop_newest.overflow_policy = OverflowPolicy::DropNewest;
    MPSCActorMailbox<TypedMessage> a(ActorId{1}, &scheduler, drop_newest);

    MailboxEnvelopeMeta meta;
    meta.type_tag = TypeTag::User;

    auto r1 = a.try_push(TypedMessage(TypeTag::User, StreamBuffer{1}), meta);
    EXPECT_TRUE(r1.accepted());

    auto r2 = a.try_push(TypedMessage(TypeTag::User, StreamBuffer{2}), meta);
    EXPECT_EQ(r2.code, EnqueueResultCode::DroppedNewest);
    EXPECT_FALSE(r2.accepted());

    // Only the first message should be in the mailbox
    TypedMessage out;
    EXPECT_TRUE(a.try_pop(out));
    EXPECT_EQ(out.payload()[0], 1);
    EXPECT_FALSE(a.try_pop(out));
}

TEST_F(OverflowPolicyTest, DropOldestEvictsOldestForNewMessage) {
    using namespace hpactor;
    using namespace hpactor::mailbox;

    MailboxConfig drop_oldest;
    drop_oldest.capacity.max_messages = 1;
    drop_oldest.overflow_policy = OverflowPolicy::DropOldest;
    MPSCActorMailbox<TypedMessage> b(ActorId{2}, &scheduler, drop_oldest);

    MailboxEnvelopeMeta meta;
    meta.type_tag = TypeTag::User;

    auto r1 = b.try_push(TypedMessage(TypeTag::User, StreamBuffer{1}), meta);
    EXPECT_TRUE(r1.accepted());

    auto r2 = b.try_push(TypedMessage(TypeTag::User, StreamBuffer{2}), meta);
    EXPECT_TRUE(r2.accepted());

    TypedMessage out;
    EXPECT_TRUE(b.try_pop(out));
    EXPECT_EQ(out.payload()[0], 2); // old was dropped, new survives
    EXPECT_FALSE(b.try_pop(out));
}

TEST_F(OverflowPolicyTest, SystemMessageUsesProtectedReserve) {
    using namespace hpactor;
    using namespace hpactor::mailbox;

    MailboxConfig reserve;
    reserve.capacity.max_messages = 1;
    reserve.protected_system_messages = 1;
    MPSCActorMailbox<TypedMessage> c(ActorId{3}, &scheduler, reserve);

    MailboxEnvelopeMeta user_meta;
    user_meta.type_tag = TypeTag::User;

    // Fill the normal capacity with a user message
    auto r1 = c.try_push(TypedMessage(TypeTag::User, StreamBuffer{1}), user_meta);
    EXPECT_TRUE(r1.accepted());

    // System message should still get through via reserve
    MailboxEnvelopeMeta sys_meta;
    sys_meta.type_tag = TypeTag::DownMsg;
    auto r2 = c.try_push(TypedMessage(TypeTag::DownMsg, StreamBuffer{9}), sys_meta);
    EXPECT_TRUE(r2.accepted());

    // Both messages should be in the mailbox
    TypedMessage out;
    EXPECT_TRUE(c.try_pop(out));
    EXPECT_TRUE(c.try_pop(out));
    EXPECT_FALSE(c.try_pop(out));
}

TEST_F(OverflowPolicyTest, DropNewestOnSystemReserveWhenFull) {
    using namespace hpactor;
    using namespace hpactor::mailbox;

    MailboxConfig cfg;
    cfg.capacity.max_messages = 1;
    cfg.protected_system_messages = 1;
    cfg.overflow_policy = OverflowPolicy::DropNewest;
    MPSCActorMailbox<TypedMessage> d(ActorId{4}, &scheduler, cfg);

    // Fill normal capacity
    MailboxEnvelopeMeta user_meta;
    user_meta.type_tag = TypeTag::User;
    auto r1 = d.try_push(TypedMessage(TypeTag::User, StreamBuffer{1}), user_meta);
    EXPECT_TRUE(r1.accepted());

    // Fill system reserve
    MailboxEnvelopeMeta sys_meta;
    sys_meta.type_tag = TypeTag::DownMsg;
    auto r2 = d.try_push(TypedMessage(TypeTag::DownMsg, StreamBuffer{2}), sys_meta);
    EXPECT_TRUE(r2.accepted());

    // Both normal and system reserve full — this system message should be
    // dropped
    auto r3 = d.try_push(TypedMessage(TypeTag::DownMsg, StreamBuffer{3}), sys_meta);
    EXPECT_EQ(r3.code, EnqueueResultCode::DroppedNewest);
}

TEST_F(OverflowPolicyTest, DeadLetterPolicyReroutes) {
    using namespace hpactor;
    using namespace hpactor::mailbox;

    MailboxConfig cfg;
    cfg.capacity.max_messages = 1;
    cfg.overflow_policy = OverflowPolicy::DeadLetter;
    MPSCActorMailbox<TypedMessage> e(ActorId{5}, &scheduler, cfg);

    MailboxEnvelopeMeta meta;
    meta.type_tag = TypeTag::User;

    auto r1 = e.try_push(TypedMessage(TypeTag::User, StreamBuffer{1}), meta);
    EXPECT_TRUE(r1.accepted());

    auto r2 = e.try_push(TypedMessage(TypeTag::User, StreamBuffer{2}), meta);
    EXPECT_EQ(r2.code, EnqueueResultCode::ReroutedToDeadLetter);
    EXPECT_FALSE(r2.accepted());
}

TEST_F(OverflowPolicyTest, SnapshotReflectsDropCounters) {
    using namespace hpactor;
    using namespace hpactor::mailbox;

    MailboxConfig cfg;
    cfg.capacity.max_messages = 1;
    cfg.overflow_policy = OverflowPolicy::DropNewest;
    MPSCActorMailbox<TypedMessage> f(ActorId{6}, &scheduler, cfg);

    MailboxEnvelopeMeta meta;
    meta.type_tag = TypeTag::User;

    f.try_push(TypedMessage(TypeTag::User, StreamBuffer{1}), meta);
    f.try_push(TypedMessage(TypeTag::User, StreamBuffer{2}),
               meta); // dropped

    auto s = f.snapshot();
    EXPECT_EQ(s.total_enqueued, 1);
    EXPECT_EQ(s.total_dropped, 1);
    EXPECT_EQ(s.total_dead_letters, 0);
}

TEST_F(OverflowPolicyTest, DropOldestFreesByteBudget) {
    using namespace hpactor;
    using namespace hpactor::mailbox;

    uint64_t sz = estimate_message_bytes(
        TypedMessage(TypeTag::User, StreamBuffer{0}));

    MailboxConfig cfg;
    cfg.capacity.max_messages = 1;
    cfg.capacity.max_bytes = sz + 10;
    cfg.overflow_policy = OverflowPolicy::DropOldest;
    MPSCActorMailbox<TypedMessage> b(ActorId{7}, &scheduler, cfg);

    MailboxEnvelopeMeta meta;
    meta.type_tag = TypeTag::User;

    // Fill the single slot and byte budget.
    auto r1 = b.try_push(
        TypedMessage(TypeTag::User, StreamBuffer{1, 2, 3, 4, 5, 6, 7, 8}), meta);
    EXPECT_TRUE(r1.accepted());

    // Second message: DropOldest evicts the first, freeing bytes for this one.
    auto r2 = b.try_push(
        TypedMessage(TypeTag::User, StreamBuffer{1, 2, 3, 4, 5, 6, 7, 8}), meta);
    EXPECT_TRUE(r2.accepted());

    TypedMessage out;
    EXPECT_TRUE(b.try_pop(out));
    EXPECT_FALSE(b.try_pop(out));
}

TEST_F(OverflowPolicyTest, RejectNewestRejectsAtCapacity) {
    using namespace hpactor;
    using namespace hpactor::mailbox;

    MailboxConfig cfg;
    cfg.capacity.max_messages = 1;
    cfg.overflow_policy = OverflowPolicy::RejectNewest;
    MPSCActorMailbox<TypedMessage> mbox(ActorId{7}, &scheduler, cfg);

    MailboxEnvelopeMeta meta;
    meta.type_tag = TypeTag::User;

    auto r1 = mbox.try_push(TypedMessage(TypeTag::User, StreamBuffer{1}), meta);
    EXPECT_TRUE(r1.accepted());

    auto r2 = mbox.try_push(TypedMessage(TypeTag::User, StreamBuffer{2}), meta);
    EXPECT_EQ(r2.code, EnqueueResultCode::Rejected);
    EXPECT_FALSE(r2.accepted());
    EXPECT_TRUE(r2.retryable());

    auto s = mbox.snapshot();
    EXPECT_EQ(s.total_enqueued, 1);
    EXPECT_EQ(s.total_rejected, 1);
    EXPECT_EQ(s.total_dropped, 0);
}

TEST_F(OverflowPolicyTest, SignalOnlyRejectsWithRetryAfter) {
    using namespace hpactor;
    using namespace hpactor::mailbox;

    MailboxConfig cfg;
    cfg.capacity.max_messages = 1;
    cfg.overflow_policy = OverflowPolicy::SignalOnly;
    cfg.signal_min_interval_ms = 250;
    MPSCActorMailbox<TypedMessage> mbox(ActorId{8}, &scheduler, cfg);

    MailboxEnvelopeMeta meta;
    meta.type_tag = TypeTag::User;

    auto r1 = mbox.try_push(TypedMessage(TypeTag::User, StreamBuffer{1}), meta);
    EXPECT_TRUE(r1.accepted());

    auto r2 = mbox.try_push(TypedMessage(TypeTag::User, StreamBuffer{2}), meta);
    EXPECT_EQ(r2.code, EnqueueResultCode::Rejected);
    EXPECT_EQ(r2.retry_after.count(), 250);

    auto s = mbox.snapshot();
    EXPECT_EQ(s.total_rejected, 1);
}

TEST_F(OverflowPolicyTest, SignalOnlySystemMessageReserveNotRejected) {
    using namespace hpactor;
    using namespace hpactor::mailbox;

    MailboxConfig cfg;
    cfg.capacity.max_messages = 1;
    cfg.protected_system_messages = 1;
    cfg.overflow_policy = OverflowPolicy::SignalOnly;
    MPSCActorMailbox<TypedMessage> mbox(ActorId{9}, &scheduler, cfg);

    // Fill normal capacity with user message
    MailboxEnvelopeMeta user_meta;
    user_meta.type_tag = TypeTag::User;
    auto r1 = mbox.try_push(TypedMessage(TypeTag::User, StreamBuffer{1}), user_meta);
    EXPECT_TRUE(r1.accepted());

    // System message uses protected reserve, not overflow policy
    MailboxEnvelopeMeta sys_meta;
    sys_meta.type_tag = TypeTag::DownMsg;
    auto r2 = mbox.try_push(TypedMessage(TypeTag::DownMsg, StreamBuffer{9}), sys_meta);
    EXPECT_TRUE(r2.accepted());

    auto s = mbox.snapshot();
    EXPECT_EQ(s.total_enqueued, 2);
    EXPECT_EQ(s.total_rejected, 0);
}

TEST_F(OverflowPolicyTest, SpillToOverflowQueueWhenFull) {
    using namespace hpactor;
    using namespace hpactor::mailbox;

    MailboxConfig cfg;
    cfg.capacity.max_messages = 1;
    cfg.overflow_policy = OverflowPolicy::SpillToOverflowQueue;
    cfg.max_overflow_depth = 16;
    MPSCActorMailbox<TypedMessage> mbox(ActorId{10}, &scheduler, cfg);

    MailboxEnvelopeMeta meta;
    meta.type_tag = TypeTag::User;

    auto r1 = mbox.try_push(TypedMessage(TypeTag::User, StreamBuffer{1}), meta);
    EXPECT_TRUE(r1.accepted());

    auto r2 = mbox.try_push(TypedMessage(TypeTag::User, StreamBuffer{2}), meta);
    EXPECT_EQ(r2.code, EnqueueResultCode::ReroutedToOverflow);
    EXPECT_TRUE(r2.retryable());

    auto s = mbox.snapshot();
    EXPECT_EQ(s.total_enqueued, 1);
    EXPECT_EQ(s.total_rejected, 0);
    EXPECT_EQ(s.overflow_depth, 1);
    EXPECT_EQ(s.overflow_total_pushed, 1);
}

TEST_F(OverflowPolicyTest, SpillToOverflowDrainsOnDequeue) {
    using namespace hpactor;
    using namespace hpactor::mailbox;

    MailboxConfig cfg;
    cfg.capacity.max_messages = 1;
    cfg.overflow_policy = OverflowPolicy::SpillToOverflowQueue;
    cfg.max_overflow_depth = 16;
    MPSCActorMailbox<TypedMessage> mbox(ActorId{11}, &scheduler, cfg);

    MailboxEnvelopeMeta meta;
    meta.type_tag = TypeTag::User;

    // Fill main mailbox
    mbox.try_push(TypedMessage(TypeTag::User, StreamBuffer{1}), meta);
    // Spill to overflow
    mbox.try_push(TypedMessage(TypeTag::User, StreamBuffer{2}), meta);

    auto s_before = mbox.snapshot();
    EXPECT_EQ(s_before.overflow_depth, 1);

    // Dequeue the main message — should drain overflow back in
    TypedMessage out;
    EXPECT_TRUE(mbox.try_pop(out));
    EXPECT_EQ(out.payload()[0], 1);

    // Second message should now be in the main mailbox
    EXPECT_TRUE(mbox.try_pop(out));
    EXPECT_EQ(out.payload()[0], 2);

    auto s_after = mbox.snapshot();
    EXPECT_EQ(s_after.overflow_depth, 0);
}

TEST_F(OverflowPolicyTest, OverflowQueueAlwaysAcceptsSpills) {
    using namespace hpactor;
    using namespace hpactor::mailbox;

    MailboxConfig cfg;
    cfg.capacity.max_messages = 1;
    cfg.overflow_policy = OverflowPolicy::SpillToOverflowQueue;
    cfg.max_overflow_depth = 1;
    MPSCActorMailbox<TypedMessage> mbox(ActorId{12}, &scheduler, cfg);

    MailboxEnvelopeMeta meta;
    meta.type_tag = TypeTag::User;

    // Fill main capacity
    mbox.try_push(TypedMessage(TypeTag::User, StreamBuffer{1}), meta);
    // First spill — overflow queue accepts
    auto r1 = mbox.try_push(TypedMessage(TypeTag::User, StreamBuffer{2}), meta);
    EXPECT_EQ(r1.code, EnqueueResultCode::ReroutedToOverflow);
    // Second spill — overflow queue still accepts (evicts oldest from overflow)
    auto r2 = mbox.try_push(TypedMessage(TypeTag::User, StreamBuffer{3}), meta);
    EXPECT_EQ(r2.code, EnqueueResultCode::ReroutedToOverflow);

    // The overflow queue always accepts; oldest in overflow may be evicted.
    auto s = mbox.snapshot();
    EXPECT_EQ(s.overflow_depth, 1);
    EXPECT_GE(s.overflow_total_pushed, 2);
}
