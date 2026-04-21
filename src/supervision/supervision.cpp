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

void SelfSupervisingActor::add_remote_child(ActorRef child) {
    remote_children_.push_back(child);
    remote_child_addresses_.push_back(child.address());
}

bool SelfSupervisingActor::has_remote_child(const ActorAddress& addr) const {
    for (const auto& child_addr : remote_child_addresses_) {
        if (child_addr == addr) return true;
    }
    return false;
}

ActorRef SelfSupervisingActor::get_remote_child(const ActorAddress& addr) const {
    for (size_t i = 0; i < remote_child_addresses_.size(); ++i) {
        if (remote_child_addresses_[i] == addr) {
            return remote_children_[i];
        }
    }
    return ActorRef{};
}

void SelfSupervisingActor::remove_remote_child(const ActorAddress& addr) {
    for (size_t i = 0; i < remote_child_addresses_.size(); ++i) {
        if (remote_child_addresses_[i] == addr) {
            remote_children_.erase(remote_children_.begin() + static_cast<std::ptrdiff_t>(i));
            remote_child_addresses_.erase(remote_child_addresses_.begin() + static_cast<std::ptrdiff_t>(i));
            return;
        }
    }
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