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

#include <hpactor/mailbox/mpsc_actor_mailbox.hpp>
#include <hpactor/msg/typed_message.hpp>
#include <hpactor/sched/scheduler.hpp>

#include <atomic>
#include <gtest/gtest.h>

using namespace hpactor;
using namespace hpactor::mailbox;

namespace {

struct MockScheduler : public sched::IScheduler {
    void start() override {}
    void stop() override {}
    void notify_ready(ActorId actor, uint8_t priority, int64_t deadline) override {
        last_actor = actor;
        last_priority = priority;
        last_deadline = deadline;
        notify_count.fetch_add(1, std::memory_order_relaxed);
    }
    void notify_idle(ActorId) override {}
    void yield(ActorId actor, uint8_t priority) override {
        notify_ready(actor, priority, INT64_MAX);
    }
    sched::TimerHandle schedule_after(sched::timer_callback, int64_t) override {
        return {};
    }
    sched::TimerHandle schedule_every(sched::timer_callback, int64_t) override {
        return {};
    }
    void cancel_timer(sched::TimerHandle) override {}
    size_t worker_count() const override {
        return 1;
    }
    bool is_running() const override {
        return true;
    }
    void register_dedicated_thread(ActorId, int) override {}
    void register_dedicated_pool(ActorId, uint32_t) override {}
    void unregister_dedicated(ActorId) override {}

    std::atomic<int> notify_count{0};
    ActorId last_actor{};
    uint8_t last_priority = 255;
    int64_t last_deadline = 0;
};

TypedMessage make_user_msg() {
    return TypedMessage(TypeTag::User, StreamBuffer{1});
}

TypedMessage make_sys_msg() {
    return TypedMessage(TypeTag::DownMsg, StreamBuffer{1});
}

MailboxEnvelopeMeta user_meta(uint8_t prio = 0) {
    MailboxEnvelopeMeta m;
    m.type_tag = TypeTag::User;
    m.priority = prio;
    return m;
}

MailboxEnvelopeMeta sys_meta() {
    MailboxEnvelopeMeta m;
    m.type_tag = TypeTag::DownMsg;
    m.priority = 0;
    return m;
}

} // namespace

// ── Tests with priority_aware = false (default) ──────────────

class PriorityLanesDefaultTest : public ::testing::Test {
  protected:
    void SetUp() override {
        cfg.capacity.max_messages = 4;
        mbox.set_config(cfg);
    }
    MailboxConfig cfg;
    MockScheduler scheduler;
    MPSCActorMailbox<TypedMessage> mbox{ActorId{1}, &scheduler, cfg};
};

TEST_F(PriorityLanesDefaultTest, AllUserMessagesToLane0FifoDequeue) {
    auto r1 = mbox.try_push(make_user_msg(), user_meta(2));
    auto r2 = mbox.try_push(make_user_msg(), user_meta(1));
    auto r3 = mbox.try_push(make_user_msg(), user_meta(0));
    EXPECT_TRUE(r1.accepted());
    EXPECT_TRUE(r2.accepted());
    EXPECT_TRUE(r3.accepted());

    auto snap = mbox.snapshot();
    EXPECT_EQ(snap.depth, 3u);
    // All messages go to lane 0 regardless of meta.priority
    EXPECT_EQ(snap.lane_depths[0], 3u);
}

// ── Tests with priority_aware = true ─────────────────────────

class PriorityLanesTest : public ::testing::Test {
  protected:
    void SetUp() override {
        cfg.capacity.max_messages = 8;
        cfg.priority_aware = true;
        cfg.priority_levels = 4;
        cfg.protected_system_messages = 2;
        mbox.set_config(cfg);
    }
    MailboxConfig cfg;
    MockScheduler scheduler;
    MPSCActorMailbox<TypedMessage> mbox{ActorId{1}, &scheduler, cfg};
};

TEST_F(PriorityLanesTest, SystemMessageGoesToSystemLane) {
    auto r = mbox.try_push(make_sys_msg(), sys_meta());
    EXPECT_TRUE(r.accepted());

    auto snap = mbox.snapshot();
    EXPECT_EQ(snap.system_lane_depth, 1u);
    EXPECT_EQ(snap.depth, 1u);
}

TEST_F(PriorityLanesTest, SystemLaneCapacityRejectsWhenFull) {
    MailboxConfig tight = cfg;
    tight.protected_system_messages = 1;
    mbox.set_config(tight);

    auto r1 = mbox.try_push(make_sys_msg(), sys_meta());
    EXPECT_TRUE(r1.accepted());

    auto r2 = mbox.try_push(make_sys_msg(), sys_meta());
    EXPECT_FALSE(r2.accepted());
    EXPECT_EQ(r2.code, EnqueueResultCode::Rejected);
}

TEST_F(PriorityLanesTest, SystemLaneIsolatedFromUserBacklog) {
    MailboxConfig tight = cfg;
    tight.capacity.max_messages = 2;
    mbox.set_config(tight);

    mbox.try_push(make_user_msg(), user_meta(0));
    mbox.try_push(make_user_msg(), user_meta(0));

    auto r = mbox.try_push(make_sys_msg(), sys_meta());
    EXPECT_TRUE(r.accepted());

    auto snap = mbox.snapshot();
    EXPECT_EQ(snap.system_lane_depth, 1u);
}

TEST_F(PriorityLanesTest, SystemMessageDequeuedBeforeUser) {
    mbox.try_push(make_user_msg(), user_meta(0));
    mbox.try_push(make_sys_msg(), sys_meta());

    TypedMessage out;
    EXPECT_TRUE(mbox.try_pop(out));
    EXPECT_TRUE(is_system_message(out.type_id()));
}

TEST_F(PriorityLanesTest, PriorityAwareRoutingToCorrectLanes) {
    mbox.try_push(make_user_msg(), user_meta(3));
    mbox.try_push(make_user_msg(), user_meta(0));
    mbox.try_push(make_user_msg(), user_meta(1));

    auto snap = mbox.snapshot();
    EXPECT_EQ(snap.lane_depths[0], 1u);
    EXPECT_EQ(snap.lane_depths[1], 1u);
    EXPECT_EQ(snap.lane_depths[2], 0u);
    EXPECT_EQ(snap.lane_depths[3], 1u);
}

TEST_F(PriorityLanesTest, PriorityAwareDequeueOrder) {
    mbox.try_push(make_user_msg(), user_meta(3));
    mbox.try_push(make_user_msg(), user_meta(1));
    mbox.try_push(make_user_msg(), user_meta(0));

    TypedMessage out;
    // P0 first
    EXPECT_TRUE(mbox.try_pop(out));
    auto snap = mbox.snapshot();
    EXPECT_EQ(snap.lane_depths[0], 0u);
    EXPECT_EQ(snap.lane_depths[1], 1u);

    // P1 second
    EXPECT_TRUE(mbox.try_pop(out));
    snap = mbox.snapshot();
    EXPECT_EQ(snap.lane_depths[1], 0u);

    // P3 last
    EXPECT_TRUE(mbox.try_pop(out));
    snap = mbox.snapshot();
    EXPECT_EQ(snap.lane_depths[3], 0u);
}

TEST_F(PriorityLanesTest, DropLowestPriorityEviction) {
    MailboxConfig tight = cfg;
    tight.capacity.max_messages = 2;
    tight.overflow_policy = OverflowPolicy::DropLowestPriority;
    mbox.set_config(tight);

    mbox.try_push(make_user_msg(), user_meta(0));
    mbox.try_push(make_user_msg(), user_meta(3));

    auto r = mbox.try_push(make_user_msg(), user_meta(1));
    EXPECT_TRUE(r.accepted());

    auto snap = mbox.snapshot();
    EXPECT_EQ(snap.lane_depths[3], 0u);
    EXPECT_GT(snap.total_dropped, 0u);
}

TEST_F(PriorityLanesTest, SnapshotPopulatesPerLaneDepths) {
    mbox.try_push(make_sys_msg(), sys_meta());
    mbox.try_push(make_user_msg(), user_meta(0));
    mbox.try_push(make_user_msg(), user_meta(2));

    auto snap = mbox.snapshot();
    EXPECT_EQ(snap.system_lane_depth, 1u);
    EXPECT_EQ(snap.lane_depths[0], 1u);
    EXPECT_EQ(snap.lane_depths[1], 0u);
    EXPECT_EQ(snap.lane_depths[2], 1u);
    EXPECT_EQ(snap.num_user_lanes, 4u);
    EXPECT_EQ(snap.high_priority_depth, 1u);
}

TEST_F(PriorityLanesTest, ConfigResizeLanesTakesEffect) {
    MailboxConfig resized = cfg;
    resized.priority_levels = 2;
    mbox.set_config(resized);

    auto snap = mbox.snapshot();
    EXPECT_EQ(snap.num_user_lanes, 2u);
}
