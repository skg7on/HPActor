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

#include <hpactor/actor/actor_state.hpp>
#include <hpactor/actor/local_actor.hpp>
#include <hpactor/core/mailbox.hpp>
#include <hpactor/mailbox/mpsc_actor_mailbox.hpp>
#include <hpactor/msg/typed_message.hpp>
#include <hpactor/sched/dispatch_policy.hpp>
#include <hpactor/types/types.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace hpactor {

// -----------------------------------------------------------------------------
// BlockingActor - actor that runs in its own thread with blocking receive.
//
// Uses DedicatedThread dispatch — each BlockingActor owns a std::thread that
// blocks on a condition variable waiting for mailbox messages.  When a message
// arrives, the mailbox continuation callback wakes the thread.
//
// Subclass and call receive(handlers...) from within your loop, or override
// on_activate() to do custom work in the dedicated thread.
// -----------------------------------------------------------------------------
class BlockingActor : public LocalActor {
  public:
    using AbstractActor::receive;

    sched::DispatchPolicy dispatch_policy() const override {
        return sched::DispatchPolicy::DedicatedThread;
    }

    void set_scheduler(sched::IScheduler* sched) override {
        scheduler_ = sched;
    }
    void set_mailbox(mailbox::MPSCActorMailbox<TypedMessage>* mbox) override {
        mailbox_ = mbox;
    }

    // Block until a message arrives, then dispatch to matching handler.
    // Handlers are called in order; the first one that accepts the message
    // consumes it.
    template <typename... Handlers> void receive(Handlers&&... handlers) {
        TypedMessage msg;
        {
            std::unique_lock<std::mutex> lock(cv_mutex_);

            // Try non-blocking pop first
            if (mailbox_ && mailbox_->try_pop(msg)) {
                message_arrived_.store(false, std::memory_order_release);
            } else {
                // Block until a message arrives
                actor_state_.set(ActorState::kIdle);
                cv_.wait(lock, [this]() {
                    return message_arrived_.load(std::memory_order_acquire) ||
                           !running_.load(std::memory_order_acquire);
                });
                if (!running_.load(std::memory_order_acquire))
                    return;
                message_arrived_.store(false, std::memory_order_release);
                actor_state_.set(ActorState::kRunning);
                if (mailbox_)
                    mailbox_->try_pop(msg);
            }
        }

        if (msg.type_id() != TypeTag::Invalid) {
            (handlers(msg), ...);
        }
    }

    // Receive messages in a loop, dispatching each to one of the handlers
    // provided via the iterator range.
    template <typename T> void receive_for(T& begin, T end) {
        while (begin != end && running_.load(std::memory_order_acquire)) {
            receive(*begin++);
        }
    }

    // Wait for one or more actors to finish.
    template <typename... Actors>
    void wait_for(ActorAddr first, Actors&&... rest) {
        // Stub: in a full implementation this would monitor exit events
        // for the given actors and block until one finishes.
        (void)first;
        ((void)rest, ...);
    }

    void await_all_other_actors_done();

    // Satisfy the pure virtual from AbstractActor.
    // Override in subclasses to handle messages delivered by thread_loop().
    void receive(TypedMessage& msg) override;

    const error& fail_state() const {
        return fail_state_;
    }
    void fail_state(error e) {
        fail_state_ = e;
    }

    void on_activate() override;
    void on_deactivate() override;

  protected:
    BlockingActor(ActorContext* ctx, ActorSystem& sys);
    BlockingActor(ActorId id, ActorContext* ctx, ActorSystem& sys);
    ~BlockingActor() override;

    mailbox::MPSCActorMailbox<TypedMessage>* get_mailbox() {
        return mailbox_;
    }
    sched::IScheduler* get_scheduler() {
        return scheduler_;
    }
    ActorState& actor_state() {
        return actor_state_;
    }

  private:
    void thread_loop();

    std::thread thread_;
    std::mutex cv_mutex_;
    std::condition_variable cv_;
    std::atomic<bool> running_{false};
    std::atomic<bool> message_arrived_{false};
    ActorState actor_state_;
    mailbox::MPSCActorMailbox<TypedMessage>* mailbox_{nullptr};
    sched::IScheduler* scheduler_{nullptr};
    error fail_state_;
};

} // namespace hpactor
