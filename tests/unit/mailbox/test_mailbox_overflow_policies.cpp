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
