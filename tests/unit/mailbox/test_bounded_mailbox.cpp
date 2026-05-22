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
