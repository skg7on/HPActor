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

// tests/unit/sched/test_mailbox_awaiter.cpp
#include <gtest/gtest.h>
#include <hpactor/actor/typed_message.hpp>
#include <hpactor/hpactor_config.hpp>
#include <hpactor/mailbox/mpsc_actor_mailbox.hpp>
#include <hpactor/sched/coroutine_awaiters.hpp>
#include <hpactor/sched/coroutine_task.hpp>
#include <hpactor/types/types.hpp>

#if HPACTOR_SUPPORT_COROUTINES

#    include <coroutine>

// Mock scheduler for MailboxAwaiter tests
struct MailboxAwaiterMockScheduler : public hpactor::sched::IScheduler {
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
    void register_dedicated_thread(hpactor::ActorId, int) override {}
    void register_dedicated_pool(hpactor::ActorId, uint32_t) override {}
    void unregister_dedicated(hpactor::ActorId) override {}
};

class MailboxAwaiterTest : public ::testing::Test {
  protected:
    void SetUp() override {
        actor_id = hpactor::ActorId{1};
        mb = std::make_unique<hpactor::mailbox::MPSCActorMailbox<hpactor::TypedMessage>>(
            actor_id, &scheduler);
    }

    MailboxAwaiterMockScheduler scheduler;
    hpactor::ActorId actor_id;
    std::unique_ptr<hpactor::mailbox::MPSCActorMailbox<hpactor::TypedMessage>> mb;
};

TEST_F(MailboxAwaiterTest, AwaitReadyFalseWhenEmpty) {
    hpactor::sched::CoroutinePromise promise;
    promise.actor_id = actor_id;
    promise.state.set(hpactor::ActorState::kRunning);

    hpactor::sched::MailboxAwaiter<hpactor::TypedMessage> awaiter(promise,
                                                                  mb.get());
    EXPECT_FALSE(awaiter.await_ready()); // mailbox empty -> don't skip suspend
}

TEST_F(MailboxAwaiterTest, AwaitReadyTrueWhenHasMessage) {
    auto* msg = new hpactor::TypedMessage(hpactor::TypeTag::User,
                                          hpactor::StreamBuffer{0x01, 0x02});
    mb->inject_for_test(msg); // inject without edge-trigger

    hpactor::sched::CoroutinePromise promise;
    promise.actor_id = actor_id;
    promise.state.set(hpactor::ActorState::kRunning);

    hpactor::sched::MailboxAwaiter<hpactor::TypedMessage> awaiter(promise,
                                                                  mb.get());
    EXPECT_TRUE(awaiter.await_ready()); // mailbox non-empty -> skip suspend

    // Clean up
    auto* popped = mb->dequeue();
    delete popped;
}

TEST_F(MailboxAwaiterTest, AwaitSuspendTransitionsRunningToIdle) {
    hpactor::sched::CoroutinePromise promise;
    promise.actor_id = actor_id;
    promise.state.set(hpactor::ActorState::kRunning);
    promise.mailbox_was_empty.store(true, std::memory_order_release);

    hpactor::sched::MailboxAwaiter<hpactor::TypedMessage> awaiter(promise,
                                                                  mb.get());

    bool did_suspend = awaiter.await_suspend(std::coroutine_handle<>{} // empty
                                                                       // handle
                                                                       // — just
                                                                       // test
                                                                       // state
                                                                       // transition
    );

    EXPECT_TRUE(did_suspend);
    EXPECT_TRUE(promise.state.is_idle());
    // Edge-trigger was reset to true (mailbox was empty at entry)
    EXPECT_TRUE(mb->was_empty());
}

TEST_F(MailboxAwaiterTest, AwaitSuspendReturnsFalseWhenTerminated) {
    hpactor::sched::CoroutinePromise promise;
    promise.actor_id = actor_id;
    promise.state.set(hpactor::ActorState::kTerminated); // not Running

    hpactor::sched::MailboxAwaiter<hpactor::TypedMessage> awaiter(promise,
                                                                  mb.get());
    bool did_suspend = awaiter.await_suspend(std::coroutine_handle<>{});
    EXPECT_FALSE(did_suspend); // should not suspend — already terminated
}

TEST_F(MailboxAwaiterTest, LostWakeupRace) {
    hpactor::sched::CoroutinePromise promise;
    promise.actor_id = actor_id;
    promise.state.set(hpactor::ActorState::kRunning);
    promise.mailbox_was_empty.store(true, std::memory_order_release);

    // Enqueue a message BEFORE await_suspend
    auto* msg = new hpactor::TypedMessage(hpactor::TypeTag::User,
                                          hpactor::StreamBuffer{0x63, 0x00});
    mb->inject_for_test(msg);

    hpactor::sched::MailboxAwaiter<hpactor::TypedMessage> awaiter(promise,
                                                                  mb.get());

    // await_ready returns true (message is in mailbox)
    EXPECT_TRUE(awaiter.await_ready());

    // await_suspend should return false because mailbox is non-empty
    // (the edge-trigger race fix: don't suspend if message arrived)
    bool did_suspend = awaiter.await_suspend(std::coroutine_handle<>{});
    EXPECT_FALSE(did_suspend); // should not suspend — message already in
                               // mailbox

    // Clean up
    auto* popped = mb->dequeue();
    delete popped;
}

#else // !HPACTOR_SUPPORT_COROUTINES

// C++17 fallback: MailboxAwaiter is not available
// This test is skipped in C++17 mode

TEST(MailboxAwaiterTest, Cpp17FallbackSkipped) {
    // MailboxAwaiter requires C++20 coroutines
    GTEST_SKIP() << "MailboxAwaiter requires C++20 coroutines";
}

#endif // HPACTOR_SUPPORT_COROUTINES
