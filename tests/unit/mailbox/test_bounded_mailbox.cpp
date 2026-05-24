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

#include <atomic>

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

class BoundedMailboxTest : public ::testing::Test {
  protected:
    void SetUp() override {
        cfg.capacity.max_messages = 2;
        cfg.high_watermark = 0.50;
        cfg.low_watermark = 0.25;
    }

    hpactor::mailbox::MailboxConfig cfg;
    MockScheduler scheduler;
};

TEST_F(BoundedMailboxTest, AcceptsMessagesUpToCapacity) {
    using namespace hpactor;
    using namespace hpactor::mailbox;

    MPSCActorMailbox<TypedMessage> mb(ActorId{77}, &scheduler, cfg);

    MailboxEnvelopeMeta meta;
    meta.type_tag = TypeTag::User;
    meta.priority = 2;
    meta.deadline_ns = 1234;

    auto r1 = mb.try_push(TypedMessage(TypeTag::User, StreamBuffer{1}), meta);
    EXPECT_TRUE(r1.accepted());
    EXPECT_EQ(r1.depth, 1);

    auto r2 = mb.try_push(TypedMessage(TypeTag::User, StreamBuffer{2}), meta);
    EXPECT_TRUE(r2.accepted());
    EXPECT_EQ(r2.depth, 2);
}

TEST_F(BoundedMailboxTest, RejectsMessageAtCapacity) {
    using namespace hpactor;
    using namespace hpactor::mailbox;

    MPSCActorMailbox<TypedMessage> mb(ActorId{77}, &scheduler, cfg);

    MailboxEnvelopeMeta meta;
    meta.type_tag = TypeTag::User;

    auto r1 = mb.try_push(TypedMessage(TypeTag::User, StreamBuffer{1}), meta);
    EXPECT_TRUE(r1.accepted());
    auto r2 = mb.try_push(TypedMessage(TypeTag::User, StreamBuffer{2}), meta);
    EXPECT_TRUE(r2.accepted());

    auto r3 = mb.try_push(TypedMessage(TypeTag::User, StreamBuffer{3}), meta);
    EXPECT_FALSE(r3.accepted());
    EXPECT_EQ(r3.code, EnqueueResultCode::Rejected);
    EXPECT_EQ(r3.capacity, 2);
}

TEST_F(BoundedMailboxTest, NotifyReadyCalledOnFirstPush) {
    using namespace hpactor;
    using namespace hpactor::mailbox;

    MPSCActorMailbox<TypedMessage> mb(ActorId{77}, &scheduler, cfg);

    MailboxEnvelopeMeta meta;
    meta.type_tag = TypeTag::User;
    meta.priority = 2;
    meta.deadline_ns = 1234;

    auto r1 = mb.try_push(TypedMessage(TypeTag::User, StreamBuffer{1}), meta);
    EXPECT_TRUE(r1.accepted());
    EXPECT_EQ(scheduler.notify_ready_count.load(), 1);
    EXPECT_EQ(scheduler.last_priority, 2);
    EXPECT_EQ(scheduler.last_deadline, 1234);
}

TEST_F(BoundedMailboxTest, PopDrainsMessagesInOrder) {
    using namespace hpactor;
    using namespace hpactor::mailbox;

    MPSCActorMailbox<TypedMessage> mb(ActorId{77}, &scheduler, cfg);

    MailboxEnvelopeMeta meta;
    meta.type_tag = TypeTag::User;

    mb.try_push(TypedMessage(TypeTag::User, StreamBuffer{1}), meta);
    mb.try_push(TypedMessage(TypeTag::User, StreamBuffer{2}), meta);

    TypedMessage out;
    EXPECT_TRUE(mb.try_pop(out));
    EXPECT_EQ(out.payload()[0], 1);
    EXPECT_TRUE(mb.try_pop(out));
    EXPECT_EQ(out.payload()[0], 2);
    EXPECT_FALSE(mb.try_pop(out));
}

TEST_F(BoundedMailboxTest, SnapshotReflectsState) {
    using namespace hpactor;
    using namespace hpactor::mailbox;

    MPSCActorMailbox<TypedMessage> mb(ActorId{77}, &scheduler, cfg);

    MailboxEnvelopeMeta meta;
    meta.type_tag = TypeTag::User;

    mb.try_push(TypedMessage(TypeTag::User, StreamBuffer{1}), meta);
    mb.try_push(TypedMessage(TypeTag::User, StreamBuffer{2}), meta);
    mb.try_push(TypedMessage(TypeTag::User, StreamBuffer{3}), meta); // rejected

    TypedMessage out;
    mb.try_pop(out);
    mb.try_pop(out);

    auto s = mb.snapshot();
    EXPECT_EQ(s.depth, 0);
    EXPECT_EQ(s.capacity, 2);
    EXPECT_EQ(s.total_enqueued, 2);
    EXPECT_EQ(s.total_rejected, 1);
}

// ---------------------------------------------------------------------------
// Byte budget tests
// ---------------------------------------------------------------------------

class ByteBudgetTest : public ::testing::Test {
  protected:
    void SetUp() override {
        cfg.capacity.max_messages = 100; // high count cap — byte budget gates
        cfg.high_watermark = 0.80;
    }

    hpactor::mailbox::MailboxConfig cfg;
    MockScheduler scheduler;
};

TEST_F(ByteBudgetTest, ByteBudgetRejectsWhenExceeded) {
    using namespace hpactor;
    using namespace hpactor::mailbox;

    // Byte budget = sizeof(TypedMessage) + 10 bytes of payload.
    uint64_t sz = estimate_message_bytes(
        TypedMessage(TypeTag::User, StreamBuffer{0})); // overhead
    cfg.capacity.max_bytes = sz + 15;

    MPSCActorMailbox<TypedMessage> mb(ActorId{77}, &scheduler, cfg);

    MailboxEnvelopeMeta meta;
    meta.type_tag = TypeTag::User;

    // First message: 10-byte payload. Should fit (sz + 10 <= sz + 15).
    auto r1 = mb.try_push(
        TypedMessage(TypeTag::User, StreamBuffer{1, 2, 3, 4, 5, 6, 7, 8, 9, 0}),
        meta);
    EXPECT_TRUE(r1.accepted());

    // Second message: 6-byte payload. Should exceed budget (sz+10 + sz+6 > sz+15).
    auto r2 = mb.try_push(
        TypedMessage(TypeTag::User, StreamBuffer{1, 2, 3, 4, 5, 6}), meta);
    EXPECT_FALSE(r2.accepted());
    EXPECT_EQ(r2.code, EnqueueResultCode::Rejected);

    // Only first message should be queued.
    TypedMessage out;
    EXPECT_TRUE(mb.try_pop(out));
    EXPECT_EQ(out.payload().size(), 10u);
    EXPECT_FALSE(mb.try_pop(out));
}

TEST_F(ByteBudgetTest, CombinedCountAndByteBudget) {
    using namespace hpactor;
    using namespace hpactor::mailbox;

    uint64_t sz = estimate_message_bytes(
        TypedMessage(TypeTag::User, StreamBuffer{0}));
    cfg.capacity.max_messages = 1; // count limit hit first
    cfg.capacity.max_bytes = sz + 1000; // generous byte limit

    MPSCActorMailbox<TypedMessage> mb(ActorId{77}, &scheduler, cfg);

    MailboxEnvelopeMeta meta;
    meta.type_tag = TypeTag::User;

    auto r1 = mb.try_push(TypedMessage(TypeTag::User, StreamBuffer{1}), meta);
    EXPECT_TRUE(r1.accepted());

    // Count limit rejects, even though byte budget is far from exhausted.
    auto r2 = mb.try_push(TypedMessage(TypeTag::User, StreamBuffer{2}), meta);
    EXPECT_FALSE(r2.accepted());
    EXPECT_EQ(r2.code, EnqueueResultCode::Rejected);
}

TEST_F(ByteBudgetTest, UnlimitedBytesPreservesCountOnly) {
    using namespace hpactor;
    using namespace hpactor::mailbox;

    cfg.capacity.max_bytes = 0; // unlimited
    cfg.capacity.max_messages = 1;

    MPSCActorMailbox<TypedMessage> mb(ActorId{77}, &scheduler, cfg);

    MailboxEnvelopeMeta meta;
    meta.type_tag = TypeTag::User;

    auto r1 = mb.try_push(TypedMessage(TypeTag::User, StreamBuffer{1}), meta);
    EXPECT_TRUE(r1.accepted());

    auto r2 = mb.try_push(TypedMessage(TypeTag::User, StreamBuffer{2}), meta);
    EXPECT_FALSE(r2.accepted());
}

TEST_F(ByteBudgetTest, DequeueReleasesBytes) {
    using namespace hpactor;
    using namespace hpactor::mailbox;

    uint64_t sz = estimate_message_bytes(
        TypedMessage(TypeTag::User, StreamBuffer{0}));
    cfg.capacity.max_bytes = sz + 15;

    MPSCActorMailbox<TypedMessage> mb(ActorId{77}, &scheduler, cfg);

    MailboxEnvelopeMeta meta;
    meta.type_tag = TypeTag::User;

    // Fill budget.
    auto r1 = mb.try_push(
        TypedMessage(TypeTag::User, StreamBuffer{1, 2, 3, 4, 5, 6, 7, 8, 9, 0}),
        meta);
    EXPECT_TRUE(r1.accepted());

    // Dequeue frees bytes.
    TypedMessage out;
    EXPECT_TRUE(mb.try_pop(out));

    // Now a message of the same size should fit again.
    auto r2 = mb.try_push(
        TypedMessage(TypeTag::User, StreamBuffer{1, 2, 3, 4, 5, 6, 7, 8, 9, 0}),
        meta);
    EXPECT_TRUE(r2.accepted());
}

TEST_F(ByteBudgetTest, SnapshotReflectsByteBudget) {
    using namespace hpactor;
    using namespace hpactor::mailbox;

    uint64_t sz = estimate_message_bytes(
        TypedMessage(TypeTag::User, StreamBuffer{0}));
    cfg.capacity.max_bytes = sz * 4; // generous — fits several messages

    MPSCActorMailbox<TypedMessage> mb(ActorId{77}, &scheduler, cfg);

    MailboxEnvelopeMeta meta;
    meta.type_tag = TypeTag::User;

    mb.try_push(TypedMessage(TypeTag::User, StreamBuffer{1, 2, 3}), meta);
    mb.try_push(TypedMessage(TypeTag::User, StreamBuffer{4, 5, 6}), meta);

    auto s = mb.snapshot();
    EXPECT_EQ(s.byte_capacity, sz * 4);
    EXPECT_GT(s.queued_bytes, 0u);
    EXPECT_EQ(s.depth, 2u);

    TypedMessage out;
    mb.try_pop(out);
    mb.try_pop(out);

    auto s2 = mb.snapshot();
    EXPECT_EQ(s2.queued_bytes, 0u);
    EXPECT_EQ(s2.depth, 0u);
}
