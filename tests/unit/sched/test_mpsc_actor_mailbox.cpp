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

// tests/unit/sched/test_mpsc_actor_mailbox.cpp
// (was tests/sched/test_actor_mailbox.cpp — renamed for clarity)

#include <gtest/gtest.h>
#include <hpactor/actor/typed_message.hpp>
#include <hpactor/mailbox/mpsc_actor_mailbox.hpp>
#include <hpactor/sched/scheduler.hpp>

#include <atomic>

using namespace hpactor;
using namespace hpactor::mailbox;

// Mock scheduler that records notify_ready calls.
// Named uniquely to avoid ODR conflicts with other test files in the same
// executable that also define mock schedulers.
struct MPSCActorMailboxMockScheduler : public hpactor::sched::IScheduler {
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

class MPSCActorMailboxTest : public ::testing::Test {
  protected:
    MPSCActorMailboxMockScheduler scheduler;
};

// Test 1: first enqueue to empty mailbox calls notify_ready
TEST_F(MPSCActorMailboxTest, FirstEnqueueCallsNotifyReady) {
    MPSCActorMailbox<TypedMessage> mb(ActorId{42}, &scheduler);

    auto r = mb.try_push(TypedMessage(TypeTag::User, StreamBuffer{0x7B}));
    EXPECT_TRUE(r.accepted());
    EXPECT_EQ(scheduler.notify_ready_count.load(), 1);
    EXPECT_EQ(scheduler.last_actor.value(), 42U);
}

// Test 2: second enqueue to non-empty mailbox does NOT call notify_ready
TEST_F(MPSCActorMailboxTest, SecondEnqueueDoesNotCallNotifyReady) {
    MPSCActorMailbox<TypedMessage> mb(ActorId{1}, &scheduler);

    auto r1 = mb.try_push(TypedMessage(TypeTag::User, StreamBuffer{0x01}));
    EXPECT_TRUE(r1.accepted());
    EXPECT_EQ(scheduler.notify_ready_count.load(), 1);

    auto r2 = mb.try_push(TypedMessage(TypeTag::User, StreamBuffer{0x02}));
    EXPECT_TRUE(r2.accepted());
    EXPECT_EQ(scheduler.notify_ready_count.load(), 1); // still 1
}

// Test 3: dequeue drains mailbox, next enqueue calls notify_ready again
TEST_F(MPSCActorMailboxTest, DequeueDrainsThenEnqueueNotifiesAgain) {
    MPSCActorMailbox<TypedMessage> mb(ActorId{1}, &scheduler);

    auto r1 = mb.try_push(TypedMessage(TypeTag::User, StreamBuffer{0x01}));
    EXPECT_TRUE(r1.accepted());
    EXPECT_EQ(scheduler.notify_ready_count.load(), 1);

    mb.dequeue();

    auto r2 = mb.try_push(TypedMessage(TypeTag::User, StreamBuffer{0x02}));
    EXPECT_TRUE(r2.accepted());
    EXPECT_EQ(scheduler.notify_ready_count.load(), 2);
}

// Test 4: push() convenience method
TEST_F(MPSCActorMailboxTest, PushConvenienceMethod) {
    MPSCActorMailbox<TypedMessage> mb(ActorId{1}, &scheduler);

    mb.push(TypedMessage(TypeTag::User, StreamBuffer{0x03, 0xE7}));
    EXPECT_EQ(scheduler.notify_ready_count.load(), 1);

    auto* node = mb.dequeue();
    ASSERT_NE(node, nullptr);
    EXPECT_FALSE(node->payload().empty());
    delete node;
}
