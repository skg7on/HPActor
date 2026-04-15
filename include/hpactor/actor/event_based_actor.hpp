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

    // Coroutine support
    const sched::CoroutineTask& get_coroutine() const { return coroutine_; }
    void set_coroutine(sched::CoroutineTask&& t) { coroutine_ = std::move(t); }

    // Mailbox state for awaiter
    // TODO: Implement when MPSC mailbox (Phase 7) is integrated
    bool mailbox_has_messages() const { return false; }
    bool mailbox_is_empty() const { return true; }

  protected:
    virtual Behavior make_behavior() {
        return {};
    }
    void on_activate() override;
    void on_deactivate() override;
    virtual void on_exit() {}

    EventBasedActor(ActorContext* ctx, ActorSystem& sys);

  private:
    sched::CoroutineTask coroutine_;
    Behavior behavior_;
};

} // namespace hpactor