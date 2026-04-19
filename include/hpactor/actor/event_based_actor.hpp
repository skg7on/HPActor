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

#include <hpactor/actor/local_actor.hpp>
#include <hpactor/behavior.hpp>
#include <hpactor/mailbox/mpsc_actor_mailbox.hpp>
#include <hpactor/sched/actor_coroutine.hpp>
#include <hpactor/sched/coroutine_awaiters.hpp>
#include <hpactor/sched/coroutine_task.hpp>

namespace hpactor {

// -----------------------------------------------------------------------------
// EventBasedActor - cooperatively scheduled actor with behavior-based
// handling and coroutine support
// -----------------------------------------------------------------------------
class EventBasedActor : public LocalActor {
  public:
    void become(Behavior bh);
    void become_empty();

    void receive(MessageVariant&& msg) override;

    // Type query for safe downcasting without RTTI
    bool is_event_based_actor() const override { return true; }

    // Coroutine support
    // act() - entry point for actor coroutine; override to implement actor logic.
    // Default returns an empty coroutine that terminates immediately.
    // Subclasses should use co_await receive() to wait for messages.
    virtual sched::CoroutineTask act() {
        co_return;  // Default empty implementation
    }

    // ActorCoroutine ownership
    sched::ActorCoroutine& get_actor_coroutine() { return actor_coroutine_; }
    const sched::ActorCoroutine& get_actor_coroutine() const { return actor_coroutine_; }
    void set_actor_coroutine(sched::ActorCoroutine&& coroutine) {
        actor_coroutine_ = std::move(coroutine);
    }

    // Accessor for scheduler (used by awaiters)
    sched::IScheduler* get_scheduler() { return scheduler_; }

    // Accessor for mailbox (used by awaiters)
    mailbox::MPSCActorMailbox<Message<MessageVariant>>* get_mailbox() { return mailbox_; }

    // Accessor for coroutine handle (set during ensure_coroutine_started)
    std::coroutine_handle<sched::CoroutinePromise> get_coro_handle() { return coro_handle_; }
    void set_coro_handle(std::coroutine_handle<sched::CoroutinePromise> h) { coro_handle_ = h; }

    // Mailbox delegation
    bool mailbox_has_messages() const {
        return mailbox_ && !mailbox_->empty();
    }
    bool mailbox_is_empty() const {
        return !mailbox_ || mailbox_->empty();
    }

    // Lazily create the actor coroutine on first execute_actor()
    void ensure_coroutine_started() {
        if (!actor_coroutine_) {
            auto task = act();
            if (task) {
                // Store coroutine handle for access to promise
                coro_handle_ = task.handle();
                actor_coroutine_ = sched::ActorCoroutine{std::move(task), id()};

                // Set the continuation callback on the mailbox to resume
                // the actor's coroutine directly when messages arrive
                if (mailbox_) {
                    auto* coro_ptr = &actor_coroutine_;
                    mailbox_->set_continuation_callback([coro_ptr]() {
                        // Only resume if coroutine exists and hasn't terminated
                        if (!coro_ptr->done()) {
                            coro_ptr->promise().notify_mailbox_nonempty();
                        }
                    });
                }
            }
        }
    }

    // Setters for runtime dependencies
    void set_scheduler(sched::IScheduler* scheduler) override { scheduler_ = scheduler; }
    void set_mailbox(mailbox::MPSCActorMailbox<Message<MessageVariant>>* mailbox) override {
        mailbox_ = mailbox;
    }

  protected:
    virtual Behavior make_behavior() {
        return {};
    }
    void on_activate() override;
    void on_deactivate() override;

  public:
    virtual void on_exit() {}

    EventBasedActor(ActorContext* ctx, ActorSystem& sys);

    // Helper to create a MailboxAwaiter for this actor's mailbox
    // Uses the stored coroutine handle to access the promise
    sched::MailboxAwaiter<Message<MessageVariant>> make_mailbox_awaiter() {
        return sched::MailboxAwaiter<Message<MessageVariant>>{
            coro_handle_.promise(),
            mailbox_
        };
    }

  private:
    sched::ActorCoroutine actor_coroutine_;
    std::coroutine_handle<sched::CoroutinePromise> coro_handle_;
    Behavior behavior_;
    mailbox::MPSCActorMailbox<Message<MessageVariant>>* mailbox_ = nullptr;
    sched::IScheduler* scheduler_ = nullptr;
};

} // namespace hpactor