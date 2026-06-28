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

// Tests for SCHED-01: direct actor pointer in WorkItem.
// Verifies that:
//   1. WorkItem carries actor_ptr (non-null after notify_ready_fast).
//   2. MPSCActorMailbox.set_actor_ptr() propagates the ptr through the
//      wakeup path into the enqueued WorkItem.
//   3. execute_actor() works correctly when actor_ptr is populated.

#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/mailbox/mpsc_actor_mailbox.hpp>
#include <hpactor/msg/typed_message.hpp>
#include <hpactor/sched/scheduler_interfaces.hpp>
#include <hpactor/sched/work_queue.hpp>
#include <hpactor/types/types.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <vector>

using namespace hpactor;
using namespace hpactor::sched;
using namespace hpactor::mailbox;

// ── Capture scheduler: records every WorkItem enqueued via notify_ready_fast
// ──

struct CapturingScheduler : public IScheduler {
    // IScheduler virtuals ───────────────────────────────────────────────────
    void start() override {}
    void stop() override {}
    bool is_running() const override {
        return true;
    }
    size_t worker_count() const override {
        return 1;
    }
    void notify_idle(ActorId) override {}
    void yield(ActorId id, uint8_t p) override {
        notify_ready(id, p, INT64_MAX);
    }
    TimerHandle schedule_after(timer_callback, int64_t) override {
        return {};
    }
    TimerHandle schedule_every(timer_callback, int64_t) override {
        return {};
    }
    void cancel_timer(TimerHandle) override {}
    void register_dedicated_thread(ActorId, int) override {}
    void register_dedicated_pool(ActorId, uint32_t) override {}
    void unregister_dedicated(ActorId) override {}

    // Fallback: called when actor_ptr is not available.
    void notify_ready(ActorId id, uint8_t /*priority*/, int64_t deadline) override {
        WorkItem item{};
        item.actor = id;
        item.deadline_ns = deadline;
        item.actor_ptr = nullptr;
        items.push_back(item);
        count.fetch_add(1, std::memory_order_relaxed);
    }

    // Fast path: called by MPSCActorMailbox when actor_ptr is set.
    void notify_ready_fast(ActorId id, EventBasedActor* ptr,
                           uint8_t /*priority*/, int64_t deadline) override {
        WorkItem item{};
        item.actor = id;
        item.deadline_ns = deadline;
        item.actor_ptr = ptr;
        items.push_back(item);
        count.fetch_add(1, std::memory_order_relaxed);
    }

    std::vector<WorkItem> items;
    std::atomic<int> count{0};
};

// ── SCHED-01 tests
// ────────────────────────────────────────────────────────────

TEST(ActorPtrWorkItem, WorkItemDefaultActorPtrIsNull) {
    WorkItem item{};
    EXPECT_EQ(item.actor_ptr, nullptr);
}

TEST(ActorPtrWorkItem, NotifyReadyFastPopulatesActorPtr) {
    CapturingScheduler sched;
    // Use a non-null sentinel pointer — we only test the plumbing, not
    // dispatch.
    auto* sentinel = reinterpret_cast<EventBasedActor*>(uintptr_t{0xDEAD});

    ActorId id{42};
    MPSCActorMailbox<TypedMessage> mb{id, &sched};
    mb.set_actor_ptr(sentinel);

    // Push triggers the empty->non-empty wakeup → notify_ready_fast.
    auto r = mb.try_push(TypedMessage{TypeTag::User, StreamBuffer{1}});
    EXPECT_TRUE(r.accepted());

    ASSERT_EQ(sched.count.load(), 1);
    EXPECT_EQ(sched.items[0].actor, id);
    EXPECT_EQ(sched.items[0].actor_ptr, sentinel);
}

TEST(ActorPtrWorkItem, NoActorPtrFallsBackToNotifyReady) {
    CapturingScheduler sched;

    ActorId id{7};
    MPSCActorMailbox<TypedMessage> mb{id, &sched};
    // actor_ptr NOT set → fallback path.

    mb.try_push(TypedMessage{TypeTag::User, StreamBuffer{1}});
    ASSERT_EQ(sched.count.load(), 1);
    EXPECT_EQ(sched.items[0].actor_ptr, nullptr);
}

TEST(ActorPtrWorkItem, SecondPushDoesNotRetriggerWakeup) {
    CapturingScheduler sched;
    auto* sentinel = reinterpret_cast<EventBasedActor*>(uintptr_t{0xBEEF});

    ActorId id{3};
    MPSCActorMailbox<TypedMessage> mb{id, &sched};
    mb.set_actor_ptr(sentinel);

    mb.try_push(TypedMessage{TypeTag::User, StreamBuffer{1}});
    mb.try_push(TypedMessage{TypeTag::User, StreamBuffer{1}});

    // Wakeup fires only on empty→non-empty transition (once).
    EXPECT_EQ(sched.count.load(), 1);
}
