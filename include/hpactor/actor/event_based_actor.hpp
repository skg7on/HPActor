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
#include <hpactor/sched/coroutine_task.hpp>

namespace hpactor {

// -----------------------------------------------------------------------------
// EventBasedActor - cooperatively scheduled actor with behavior-based
// handling
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
    virtual sched::CoroutineTask act() {
        return {};  // Empty coroutine - subclasses override with their impl
    }

    // ActorCoroutine ownership
    sched::ActorCoroutine& get_actor_coroutine() { return actor_coroutine_; }
    void set_actor_coroutine(sched::ActorCoroutine&& coroutine) {
        actor_coroutine_ = std::move(coroutine);
    }

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
            actor_coroutine_ = sched::ActorCoroutine{act(), id()};
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

  private:
    sched::ActorCoroutine actor_coroutine_;
    Behavior behavior_;
    mailbox::MPSCActorMailbox<Message<MessageVariant>>* mailbox_ = nullptr;
    sched::IScheduler* scheduler_ = nullptr;
};

} // namespace hpactor