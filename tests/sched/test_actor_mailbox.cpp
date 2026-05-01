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

#include <cassert>
#include <hpactor/actor/typed_message.hpp>
#include <hpactor/mailbox/mpsc_actor_mailbox.hpp>
#include <hpactor/sched/scheduler.hpp>

// Mock scheduler that records notify_ready calls
struct MockScheduler : public hpactor::sched::IScheduler {
    MockScheduler() : notify_ready_count(0), last_actor{} {}

    void start() override {}
    void stop() override {}
    void notify_ready(hpactor::ActorId actor, uint8_t, int64_t) override {
        notify_ready_count.fetch_add(1, std::memory_order_relaxed);
        last_actor = actor;
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

    std::atomic<int> notify_ready_count;
    hpactor::ActorId last_actor;
};

int main() {
    // Test 1: first enqueue to empty mailbox calls notify_ready
    {
        MockScheduler scheduler;
        hpactor::mailbox::MPSCActorMailbox<hpactor::TypedMessage> mb(
            hpactor::ActorId{42}, &scheduler);
        assert(scheduler.notify_ready_count.load() == 0);
        auto* msg = new hpactor::TypedMessage(hpactor::TypeTag::User, hpactor::StreamBuffer{0x7B});
        mb.enqueue(msg);
        assert(scheduler.notify_ready_count.load() == 1);
        assert(scheduler.last_actor.value() == 42);
    }

    // Test 2: second enqueue to non-empty mailbox does NOT call notify_ready
    {
        MockScheduler scheduler;
        hpactor::mailbox::MPSCActorMailbox<hpactor::TypedMessage> mb(
            hpactor::ActorId{1}, &scheduler);
        auto* msg1 = new hpactor::TypedMessage(hpactor::TypeTag::User, hpactor::StreamBuffer{0x01});
        auto* msg2 = new hpactor::TypedMessage(hpactor::TypeTag::User, hpactor::StreamBuffer{0x02});
        mb.enqueue(msg1);
        assert(scheduler.notify_ready_count.load() == 1);
        mb.enqueue(msg2);
        assert(scheduler.notify_ready_count.load() == 1); // still 1
    }

    // Test 3: dequeue drains mailbox, next enqueue calls notify_ready again
    {
        MockScheduler scheduler;
        hpactor::mailbox::MPSCActorMailbox<hpactor::TypedMessage> mb(
            hpactor::ActorId{1}, &scheduler);
        auto* msg1 = new hpactor::TypedMessage(hpactor::TypeTag::User, hpactor::StreamBuffer{0x01});
        auto* msg2 = new hpactor::TypedMessage(hpactor::TypeTag::User, hpactor::StreamBuffer{0x02});
        mb.enqueue(msg1);
        assert(scheduler.notify_ready_count.load() == 1);
        mb.dequeue();
        mb.enqueue(msg2);
        assert(scheduler.notify_ready_count.load() == 2);
    }

    // Test 4: push() convenience method
    {
        MockScheduler scheduler;
        hpactor::mailbox::MPSCActorMailbox<hpactor::TypedMessage> mb(
            hpactor::ActorId{1}, &scheduler);
        mb.push(hpactor::TypedMessage(hpactor::TypeTag::User, hpactor::StreamBuffer{0x03, 0xE7}));
        assert(scheduler.notify_ready_count.load() == 1);
        auto* node = mb.dequeue();
        assert(node != nullptr);
        assert(!node->payload().empty());
        delete node;
    }

    return 0;
}