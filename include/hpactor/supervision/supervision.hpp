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
#include <chrono>
#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/actor_context.hpp>
#include <hpactor/ref/actor_address.hpp>
#include <hpactor/ref/actor_ref.hpp>
#include <hpactor/types/types.hpp>
#include <unordered_map>
#include <vector>

namespace hpactor {

// SupervisionDirective - directive from supervisor to child on failure
enum class SupervisionDirective { Restart, Stop, Escalate };

// ChildFailure - information about a child actor failure
struct ChildFailure {
    ActorId child_id;
    error reason;
    SupervisionDirective directive;
};

// SupervisionPolicy - configuration for supervision behavior
struct SupervisionPolicy {
    enum class Strategy { OneForOne, AllForOne, OneForAll };
    Strategy strategy = Strategy::OneForOne;
    uint32_t max_restarts = 10;
    std::chrono::milliseconds restart_interval{5000};
};

// Supervisor - interface for supervision strategy
class Supervisor {
  public:
    virtual ~Supervisor() = default;
    virtual SupervisionDirective on_child_failure(const ChildFailure& failure) = 0;
    virtual void on_child_stopped(ActorId child_id);
};

// SupervisorActor - EventBasedActor that implements supervision
class SupervisorActor : public EventBasedActor {
  public:
    SupervisorActor(ActorContext* ctx, ActorSystem& sys, Supervisor& strategy,
                    std::vector<Actor> children);

  protected:
    Behavior make_behavior() override;

    // Override to add actual spawn logic for restarted children.
    // The base implementation manages restart counts and the sliding window.
    virtual void restart_child(ActorId child_id);
    void restart_all_children();

    Supervisor& strategy_;
    std::vector<Actor> children_;
    std::unordered_map<ActorId, uint32_t> restart_counts_;
    std::chrono::steady_clock::time_point first_failure_time_;

  private:
    void handle_child_down(TypeTag tag, const bytes& payload);
};

// SelfSupervisingActor - EventBasedActor that manages its own children
class SelfSupervisingActor : public EventBasedActor {
  public:
    SelfSupervisingActor(ActorContext* ctx, ActorSystem& sys,
                         SupervisionPolicy policy = SupervisionPolicy{});

    void add_child(Actor child);
    void remove_child(Actor child);

    // Remote child management
    void add_remote_child(ActorRef child);
    bool has_remote_child(const ActorAddress& addr) const;
    ActorRef get_remote_child(const ActorAddress& addr) const;
    void remove_remote_child(const ActorAddress& addr);
    const std::vector<ActorRef>& remote_children() const {
        return remote_children_;
    }

  protected:
    virtual SupervisionDirective on_failure(ActorId child_id, const error& err);
    void handle_child_down(TypeTag tag, const bytes& payload);

  private:
    SupervisionDirective decide_restart(ActorId child_id, const error& err);
    std::vector<Actor> children_;
    SupervisionPolicy policy_;
    std::unordered_map<ActorId, uint32_t> restart_counts_;
    std::chrono::steady_clock::time_point first_failure_time_;
    std::vector<ActorRef> remote_children_; // remote child references
    std::vector<ActorAddress> remote_child_addresses_; // for persistence
};

} // namespace hpactor