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

// tests/sched/test_mailbox_awaiter.cpp
#include <cassert>
#include <hpactor/actor/message.hpp>
#include <hpactor/hpactor_config.hpp>
#include <hpactor/mailbox/mpsc_actor_mailbox.hpp>
#include <hpactor/sched/coroutine_awaiters.hpp>
#include <hpactor/sched/coroutine_task.hpp>
#include <hpactor/types/types.hpp>

#if HPACTOR_USE_COROUTINES

#    include <coroutine>

// Mock scheduler for MailboxAwaiter tests
struct MockScheduler : public hpactor::sched::IScheduler {
    void start() override {}
    void stop() override {}
    void notify_ready(hpactor::ActorId, uint8_t, int64_t) override {}
    void notify_idle(hpactor::ActorId) override {}
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
    void yield(hpactor::ActorId, uint8_t) override {}
};

int main() {
    MockScheduler scheduler;
    hpactor::ActorId actor_id{1};
    hpactor::mailbox::MPSCActorMailbox<hpactor::Message<int>> mb(actor_id,
                                                                 &scheduler);

    // Test 1: await_ready() returns false when mailbox is empty
    {
        hpactor::sched::CoroutinePromise promise;
        promise.actor_id = actor_id;
        promise.state.set(hpactor::ActorState::kRunning);

        hpactor::sched::MailboxAwaiter<hpactor::Message<int>> awaiter(promise, &mb);
        assert(!awaiter.await_ready()); // mailbox empty → don't skip suspend
    }

    // Test 2: await_ready() returns true when mailbox has message
    {
        auto* msg = new hpactor::Message<int>(42);
        mb.inject_for_test(msg); // inject without edge-trigger

        hpactor::sched::CoroutinePromise promise;
        promise.actor_id = actor_id;
        promise.state.set(hpactor::ActorState::kRunning);

        hpactor::sched::MailboxAwaiter<hpactor::Message<int>> awaiter(promise, &mb);
        assert(awaiter.await_ready()); // mailbox non-empty → skip suspend

        // Clean up
        auto* popped = mb.dequeue();
        delete popped;
    }

    // Test 3: await_suspend() transitions Running→Idle and resets edge-trigger
    {
        hpactor::sched::CoroutinePromise promise;
        promise.actor_id = actor_id;
        promise.state.set(hpactor::ActorState::kRunning);
        promise.mailbox_was_empty.store(true, std::memory_order_release);

        hpactor::sched::MailboxAwaiter<hpactor::Message<int>> awaiter(promise, &mb);

        // await_suspend should CAS Running→Idle and reset edge-trigger
        bool did_suspend =
            awaiter.await_suspend(std::coroutine_handle<>{} // empty handle — we
                                                            // just test state
                                                            // transition
            );

        assert(did_suspend);
        assert(promise.state.is_idle());
        // Edge-trigger was reset to true (mailbox was empty at entry)
        assert(mb.was_empty());
    }

    // Test 4: await_suspend() returns false when state is not Running
    // (terminated)
    {
        hpactor::sched::CoroutinePromise promise;
        promise.actor_id = actor_id;
        promise.state.set(hpactor::ActorState::kTerminated); // not Running

        hpactor::sched::MailboxAwaiter<hpactor::Message<int>> awaiter(promise, &mb);
        bool did_suspend = awaiter.await_suspend(std::coroutine_handle<>{});
        assert(!did_suspend); // should not suspend — already terminated
    }

    // Test 5: lost wakeup race — message arrives between await_ready and
    // await_suspend
    {
        hpactor::sched::CoroutinePromise promise;
        promise.actor_id = actor_id;
        promise.state.set(hpactor::ActorState::kRunning);
        promise.mailbox_was_empty.store(true, std::memory_order_release);

        // Enqueue a message BEFORE await_suspend
        auto* msg = new hpactor::Message<int>(99);
        mb.inject_for_test(msg);

        hpactor::sched::MailboxAwaiter<hpactor::Message<int>> awaiter(promise, &mb);

        // await_ready returns true (message is in mailbox)
        assert(awaiter.await_ready());

        // await_suspend should return false because mailbox is non-empty
        // (the edge-trigger race fix: don't suspend if message arrived)
        bool did_suspend = awaiter.await_suspend(std::coroutine_handle<>{});
        assert(!did_suspend); // should not suspend — message already in mailbox

        // Clean up
        auto* popped = mb.dequeue();
        delete popped;
    }

    return 0;
}

#else // !HPACTOR_USE_COROUTINES

// C++17 fallback: MailboxAwaiter is not available
// This test is skipped in C++17 mode

int main() {
    // MailboxAwaiter requires C++20 coroutines
    return 0;
}

#endif // HPACTOR_USE_COROUTINES
