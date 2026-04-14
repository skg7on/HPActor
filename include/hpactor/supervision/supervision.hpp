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

  private:
    void handle_child_down(const down_msg& msg);
    void restart_child(ActorId child_id);
    void restart_all_children();
    Supervisor& strategy_;
    std::vector<Actor> children_;
    std::unordered_map<ActorId, uint32_t> restart_counts_;
    std::chrono::steady_clock::time_point first_failure_time_;
};

// SelfSupervisingActor - EventBasedActor that manages its own children
class SelfSupervisingActor : public EventBasedActor {
  public:
    SelfSupervisingActor(ActorContext* ctx, ActorSystem& sys,
                           SupervisionPolicy policy = SupervisionPolicy{});

    void add_child(Actor child);
    void remove_child(Actor child);

  protected:
    virtual SupervisionDirective on_failure(ActorId child_id, const error& err);

  private:
    void handle_child_down(const down_msg& msg);
    SupervisionDirective decide_restart(ActorId child_id, const error& err);
    std::vector<Actor> children_;
    SupervisionPolicy policy_;
    std::unordered_map<ActorId, uint32_t> restart_counts_;
    std::chrono::steady_clock::time_point first_failure_time_;
};

} // namespace hpactor