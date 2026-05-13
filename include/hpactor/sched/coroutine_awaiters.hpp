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

#pragma once

#include <hpactor/actor/typed_message.hpp>
#include <hpactor/hpactor_config.hpp>
#include <hpactor/mailbox/mpsc_actor_mailbox.hpp>
#include <hpactor/sched/coroutine_task.hpp>
#include <hpactor/sched/scheduler.hpp>

#include <atomic>
#include <cstdint>

#if HPACTOR_SUPPORT_COROUTINES

#    include <coroutine>

namespace hpactor::sched {

// MailboxAwaiter: awaitable for co_await actor.receive()
// Suspends when mailbox is empty, resumes when message arrives
// T is the message type (e.g., Message<MessageVariant>)
template <typename T> class MailboxAwaiter {
  public:
    explicit MailboxAwaiter(CoroutinePromise& promise,
                            mailbox::MPSCActorMailbox<T>* mailbox) noexcept
        : promise_(promise), mailbox_(mailbox) {}

    // Return true if message already available (don't suspend)
    bool await_ready() const noexcept {
        // Check if message arrived between last suspension and now
        return !mailbox_->was_empty();
    }

    // Called when suspending
    bool await_suspend(std::coroutine_handle<> continuation) noexcept {
        // Check emptiness at this moment — a message may have arrived since
        // await_ready(). If a message arrived while we were deciding, the
        // sender already claimed the wakeup via CAS(true, false) on was_empty —
        // don't suspend.
        bool was_empty = mailbox_->was_empty();
        if (!was_empty) {
            return false;
        }

        // Mailbox is still empty — safely reset edge-trigger so the next
        // enqueue (after we suspend) can claim the wakeup.
        mailbox_->set_was_empty(true);

        // Transition: Running → Idle
        uint32_t expected = ActorState::kRunning;
        if (promise_.actor_state->cas(expected, ActorState::kIdle)) {
            promise_.continuation = continuation;
            return true; // successfully suspended
        }
        // State was not Running — actor may have already terminated
        return false; // don't suspend
    }

    // Called when resuming (message arrived)
    T await_resume() noexcept {
        // Dequeue and return the message
        auto* msg = mailbox_->dequeue();
        if (msg) {
            // Return by moving the Message out
            return std::move(*msg);
        }
        // Return empty message if dequeue failed
        return T{};
    }

  private:
    CoroutinePromise& promise_;
    mailbox::MPSCActorMailbox<T>* mailbox_;
};

// TimerAwaiter: awaitable for co_await scheduler.schedule_after(delay)
// Wires to HybridScheduler::schedule_timer() for real timer integration
class TimerAwaiter {
  public:
    TimerAwaiter(int64_t delay_ns, HybridScheduler& scheduler, ActorId actor_id,
                 uint8_t priority = 0) noexcept
        : scheduler_(scheduler), actor_id_(actor_id), delay_ns_(delay_ns),
          priority_(priority) {}

    bool await_ready() const noexcept {
        return false;
    }

    bool await_suspend(std::coroutine_handle<> continuation) noexcept {
        continuation_ = continuation;

        // Set promise to IOWaiting
        auto& promise = std::coroutine_handle<CoroutinePromise>::from_address(
                            continuation.address())
                            .promise();
        promise.set_io_waiting();

        // Schedule timer — on expiry, actor is re-woken via notify_ready
        timer_id_ = scheduler_.schedule_timer(delay_ns_, [this] {
            scheduler_.notify_ready(actor_id_, priority_, INT64_MAX);
        });

        return true;
    }

    void await_resume() noexcept {
        // Timer fired; actor has been re-woken
    }

    void await_cancel() noexcept {
        scheduler_.cancel_timer(TimerHandle{timer_id_});
    }

  private:
    HybridScheduler& scheduler_;
    ActorId actor_id_;
    int64_t delay_ns_;
    uint8_t priority_;
    uint64_t timer_id_{0};
    std::coroutine_handle<> continuation_;
};

// BlockingMailboxAwaiter: for blocking receive with stackful coroutines
// T is the message type (e.g., Message<MessageVariant>)
template <typename T> class BlockingMailboxAwaiter {
  public:
    BlockingMailboxAwaiter(CoroutinePromise& promise,
                           mailbox::MPSCActorMailbox<T>* mailbox,
                           std::coroutine_handle<> continuation) noexcept
        : promise_(promise), mailbox_(mailbox), continuation_(continuation) {}

    bool await_ready() const noexcept {
        return !mailbox_->was_empty();
    }

    bool await_suspend(std::coroutine_handle<> continuation) noexcept {
        // Check emptiness at this moment — a message may have arrived since
        // await_ready()
        bool was_empty = mailbox_->was_empty();
        if (!was_empty)
            return false; // message arrived between await_ready() and here

        // Only reset edge-trigger if mailbox was empty at entry.
        // If a message arrived while we were deciding, the sender already
        // claimed the wakeup via CAS(true, false) on was_empty.
        if (was_empty) {
            mailbox_->set_was_empty(true);
        }

        promise_.continuation = continuation;
        promise_.set_idle();
        return true;
    }

    void await_resume() noexcept {
        // Returns the message
    }

  private:
    CoroutinePromise& promise_;
    mailbox::MPSCActorMailbox<T>* mailbox_;
    std::coroutine_handle<> continuation_;
};

} // namespace hpactor::sched

#endif // HPACTOR_SUPPORT_COROUTINES
