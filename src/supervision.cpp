#include <hpactor/supervision/all_for_one_supervisor.hpp>
#include <hpactor/supervision/one_for_one_supervisor.hpp>
#include <hpactor/supervision/supervision.hpp>

namespace hpactor {

void Supervisor::on_child_stopped(ActorId /*child_id*/) {
    // Default implementation does nothing
}

OneForOneSupervisor::OneForOneSupervisor(SupervisionPolicy policy)
    : policy_(std::move(policy)) {}

SupervisionDirective
OneForOneSupervisor::on_child_failure(const ChildFailure& failure) {
    // OneForOne: only the failed child is affected
    return failure.directive;
}

AllForOneSupervisor::AllForOneSupervisor(SupervisionPolicy policy)
    : policy_(std::move(policy)) {}

SupervisionDirective
AllForOneSupervisor::on_child_failure(const ChildFailure& /*failure*/) {
    // AllForOne: failure affects all children, restart all
    return SupervisionDirective::Restart;
}

// SupervisorActor implementation
SupervisorActor::SupervisorActor(ActorContext* ctx, ActorSystem& sys,
                                   Supervisor& strategy, std::vector<Actor> children)
    : EventBasedActor(ctx, sys), strategy_(strategy),
      children_(std::move(children)),
      first_failure_time_(std::chrono::steady_clock::time_point::min()) {}

Behavior SupervisorActor::make_behavior() {
    return Behavior{[this](MessageVariant msg) {
        std::visit(
            [this](auto&& m) {
                using T = std::decay_t<decltype(m)>;
                if constexpr (std::is_same_v<T, down_msg>) {
                    handle_child_down(m);
                }
            },
            msg);
    }};
}

void SupervisorActor::handle_child_down(const down_msg& msg) {
    auto child_id = msg.terminated_actor.id;

    ChildFailure failure{child_id, msg.reason, SupervisionDirective::Restart};

    auto directive = strategy_.on_child_failure(failure);

    switch (directive) {
        case SupervisionDirective::Restart:
            restart_child(child_id);
            break;
        case SupervisionDirective::Stop:
            children_.erase(std::remove_if(children_.begin(), children_.end(),
                                           [&child_id](const Actor& a) {
                                               return a.id() == child_id;
                                           }),
                            children_.end());
            break;
        case SupervisionDirective::Escalate:
            // TODO: escalate to parent supervisor
            break;
    }
}

void SupervisorActor::restart_child(ActorId child_id) {
    auto now = std::chrono::steady_clock::now();
    auto& count = restart_counts_[child_id];

    if (now - first_failure_time_ > std::chrono::milliseconds(5000)) {
        count = 0;
        first_failure_time_ = now;
    }

    if (count >= 10) {
        children_.erase(std::remove_if(children_.begin(), children_.end(),
                                       [&child_id](const Actor& a) {
                                           return a.id() == child_id;
                                       }),
                        children_.end());
        restart_counts_.erase(child_id);
        return;
    }

    ++count;
    // TODO: Actual restart would recreate and add the child actor
}

void SupervisorActor::restart_all_children() {
    for (auto& child : children_) {
        restart_child(child.id());
    }
}

// SelfSupervisingActor implementation
SelfSupervisingActor::SelfSupervisingActor(ActorContext* ctx, ActorSystem& sys,
                                               SupervisionPolicy policy)
    : EventBasedActor(ctx, sys), policy_(std::move(policy)),
      first_failure_time_(std::chrono::steady_clock::time_point::min()) {}

void SelfSupervisingActor::add_child(Actor child) {
    children_.push_back(std::move(child));
}

void SelfSupervisingActor::remove_child(Actor child) {
    children_.erase(std::remove_if(children_.begin(), children_.end(),
                                   [&child](const Actor& a) {
                                       return a.address() == child.address();
                                   }),
                    children_.end());
}

SupervisionDirective
SelfSupervisingActor::on_failure(ActorId child_id, const error& err) {
    return decide_restart(child_id, err);
}

void SelfSupervisingActor::handle_child_down(const down_msg& msg) {
    decide_restart(msg.terminated_actor.id, msg.reason);
}

SupervisionDirective
SelfSupervisingActor::decide_restart(ActorId child_id, const error& err) {
    auto now = std::chrono::steady_clock::now();
    auto& count = restart_counts_[child_id];

    if (now - first_failure_time_ > policy_.restart_interval) {
        count = 0;
        first_failure_time_ = now;
    }

    if (count >= policy_.max_restarts) {
        return SupervisionDirective::Stop;
    }

    ++count;
    return on_failure(child_id, err);
}

} // namespace hpactor