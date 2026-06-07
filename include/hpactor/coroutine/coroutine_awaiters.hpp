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

#include <hpactor/coroutine/coroutine_task.hpp>
#include <hpactor/hpactor_config.hpp>
#include <hpactor/mailbox/mpsc_actor_mailbox.hpp>
#include <hpactor/metrics/metrics_ring_buffer.hpp>
#include <hpactor/msg/dead_letter_record.hpp>
#include <hpactor/msg/failure_reason.hpp>
#include <hpactor/msg/typed_message.hpp>
#include <hpactor/sched/scheduler_interfaces.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>

#if HPACTOR_SUPPORT_COROUTINES

#    include <coroutine>

namespace hpactor::sched {

// MailboxAwaiter: awaitable for co_await actor.receive()
// Suspends when mailbox is empty, resumes when message arrives
// T is the message type (e.g., TypedMessage)
template <typename T> class MailboxAwaiter {
  public:
    explicit MailboxAwaiter(
        CoroutinePromise& promise, mailbox::MPSCActorMailbox<T>* mailbox,
        mailbox::DeadLetterQueue* dlq = nullptr,
        metrics::MpscRingBuffer<metrics::MetricEvent>* metrics = nullptr,
        ActorId actor_id = ActorId{}) noexcept
        : promise_(promise), mailbox_(mailbox), dlq_(dlq), metrics_(metrics),
          actor_id_(actor_id) {}

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
        while (true) {
            auto* msg = mailbox_->dequeue();
            if (!msg) {
                return T{}; // Mailbox empty
            }

            // Check message deadline before returning to the handler.
            int64_t deadline = msg->deadline_ns();
            if (deadline != INT64_MAX) {
                uint64_t now_ns = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count());
                if (mailbox::is_expired(deadline, now_ns)) {
                    // Emit metric
                    if (metrics_) {
                        metrics::MetricEvent evt{};
                        evt.timestamp_ns = now_ns;
                        evt.actor_id = actor_id_;
                        evt.event_type = metrics::MetricEventType::kDeliveryExpired;
                        evt.code = static_cast<uint8_t>(FailureReason::Expired);
                        evt.value_hi = 1;
                        metrics_->try_push(evt);
                    }

                    // Record to dead-letter queue
                    if (dlq_ && dlq_->config().enabled) {
                        mailbox::DeadLetterRecord dl;
                        dl.reason = mailbox::DeadLetterReason::Expired;
                        dl.source = mailbox::DeadLetterSource::LocalDelivery;
                        dl.sender = msg->sender_address();
                        dl.type_tag = msg->type_id();
                        dl.deadline_ns = deadline;
                        dl.payload_sample = msg->payload();
                        dl.timestamp_ns = now_ns;
                        if (msg->has_trace_context()) {
                            auto& tc = msg->trace_context();
                            std::memcpy(&dl.trace_id_hi,
                                        tc.trace_id.bytes.data(), 8);
                            std::memcpy(&dl.trace_id_lo,
                                        tc.trace_id.bytes.data() + 8, 8);
                            std::memcpy(&dl.span_id, tc.span_id.bytes.data(), 8);
                        }
                        (void)dlq_->try_push(std::move(dl));
                    }

                    // Destroy expired message and loop for next
                    msg->~T();
                    mem::deallocate(msg);
                    continue;
                }
            }

            // Message is valid — return to handler.
            return std::move(*msg);
        }
    }

  private:
    CoroutinePromise& promise_;
    mailbox::MPSCActorMailbox<T>* mailbox_;
    mailbox::DeadLetterQueue* dlq_{nullptr};
    metrics::MpscRingBuffer<metrics::MetricEvent>* metrics_{nullptr};
    ActorId actor_id_{};
};

// TimerAwaiter: awaitable for co_await scheduler.schedule_after(delay)
// Wires to HybridScheduler::schedule_timer() for real timer integration
class TimerAwaiter {
  public:
    TimerAwaiter(int64_t delay_ns, ITimerService& timer_service,
                 IActorReadyNotifier& ready_notifier, ActorId actor_id,
                 uint8_t priority = 0) noexcept
        : timer_service_(timer_service), ready_notifier_(ready_notifier),
          actor_id_(actor_id), delay_ns_(delay_ns), priority_(priority) {}

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
        timer_handle_ = timer_service_.schedule_after(
            [this] {
                ready_notifier_.notify_ready(actor_id_, priority_, INT64_MAX);
            },
            delay_ns_);

        return true;
    }

    void await_resume() noexcept {
        // Timer fired; actor has been re-woken
    }

    void await_cancel() noexcept {
        timer_service_.cancel_timer(timer_handle_);
    }

  private:
    ITimerService& timer_service_;
    IActorReadyNotifier& ready_notifier_;
    ActorId actor_id_;
    int64_t delay_ns_;
    uint8_t priority_;
    TimerHandle timer_handle_{};
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
