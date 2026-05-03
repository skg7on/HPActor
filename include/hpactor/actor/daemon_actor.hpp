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

#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/sched/dispatch_policy.hpp>

#include <atomic>
#include <thread>

namespace hpactor {

class DaemonActor : public EventBasedActor {
  public:
    sched::DispatchPolicy dispatch_policy() const override {
        return sched::DispatchPolicy::DedicatedThread;
    }

    // Override to provide the daemon's main loop body.
    // Called repeatedly from the dedicated thread.
    // Return false to exit the loop (actor is shutting down).
    virtual bool run_once() = 0;

    // Called when the dedicated thread starts, before run_once loop.
    virtual void on_daemon_start() {}

    // Called when the dedicated thread stops, after run_once loop.
    virtual void on_daemon_stop() {}

    void set_cpu_affinity(int core) {
        hints_.cpu_affinity = core;
    }

    sched::DispatchHints dispatch_hints() const override {
        return hints_;
    }

    // Access the mailbox for draining in run_once
    mailbox::MPSCActorMailbox<TypedMessage>* mailbox() {
        return get_mailbox();
    }

    void on_activate() override;
    void on_deactivate() override;

  protected:
    DaemonActor(ActorContext* ctx, ActorSystem& sys);
    ~DaemonActor() override;

  private:
    void daemon_loop();

    std::thread daemon_thread_;
    std::atomic<bool> running_{false};
    sched::DispatchHints hints_;
};

} // namespace hpactor
