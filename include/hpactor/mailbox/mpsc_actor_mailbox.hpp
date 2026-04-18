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

#include <hpactor/mailbox/mpsc_mailbox.hpp>
#include <hpactor/sched/scheduler.hpp>
#include <hpactor/actor/message.hpp>

#include <atomic>

namespace hpactor::mailbox {

template<typename T>
class MPSCActorMailbox {
public:
    MPSCActorMailbox(ActorId actor_id, sched::IScheduler* scheduler) noexcept
        : actor_id_(actor_id), scheduler_(scheduler) {}

    // Producer: enqueue message and potentially wake actor (edge-trigger)
    void enqueue(T* node) noexcept {
        bool was_empty = empty();
        mailbox_.enqueue(node);
        if (was_empty) {
            bool expected = true;
            if (mailbox_was_empty_.compare_exchange_strong(
                    expected, false,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                scheduler_->notify_ready(actor_id_, 0, INT64_MAX);
            }
        }
    }

    // Convenience: enqueue from a Message<T> rvalue (heap-allocates)
    void push(T&& msg) noexcept {
        auto* node = new T(std::move(msg));
        enqueue(node);
    }

    // Consumer: dequeue one message
    T* dequeue() noexcept {
        T* node = mailbox_.dequeue();
        if (node != nullptr && empty()) {
            mailbox_was_empty_.store(true, std::memory_order_release);
        }
        return node;
    }

    // Non-blocking pop matching ActorMailbox interface
    bool try_pop(T& out) noexcept {
        T* node = dequeue();
        if (!node) return false;
        out = std::move(*node);
        delete node;
        return true;
    }

    bool empty() const noexcept { return mailbox_.empty(); }

    // For MailboxAwaiter: was_empty before suspension?
    bool was_empty() const noexcept {
        return mailbox_was_empty_.load(std::memory_order_acquire);
    }

    // Reset edge-trigger (called when actor suspends via await_suspend)
    void set_was_empty(bool val) noexcept {
        mailbox_was_empty_.store(val, std::memory_order_release);
    }

    // Inject a message for testing (bypasses scheduler notify_ready)
    void inject_for_test(T* node) noexcept {
        mailbox_.enqueue(node);
        mailbox_was_empty_.store(false, std::memory_order_release);
    }

private:
    ActorId actor_id_;
    sched::IScheduler* scheduler_;
    MPSCMailbox<T> mailbox_;
    std::atomic<bool> mailbox_was_empty_{true};
};

} // namespace hpactor::mailbox