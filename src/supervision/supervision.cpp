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

#include <hpactor/actor/lifecycle_actor.hpp>
#include <hpactor/messages.pb.h>
#include <hpactor/metrics/metrics_event.hpp>

namespace hpactor {

void Supervisor::on_child_stopped(ActorId /*child_id*/) {}

OneForOneSupervisor::OneForOneSupervisor(SupervisionPolicy policy)
    : policy_(policy) {}

SupervisionDirective
OneForOneSupervisor::on_child_failure(const ChildFailure& failure) {
    return failure.directive;
}

AllForOneSupervisor::AllForOneSupervisor(SupervisionPolicy policy)
    : policy_(policy) {}

SupervisionDirective
AllForOneSupervisor::on_child_failure(const ChildFailure& /*failure*/) {
    return SupervisionDirective::Restart;
}

SupervisorActor::SupervisorActor(ActorContext* ctx, ActorSystem& sys,
                                 Supervisor& strategy, std::vector<Actor> children)
    : EventBasedActor(ctx, sys), strategy_(strategy),
      children_(mem::MemStdAllocator<Actor>(id_ptr(), mem::RegionType::kActor)),
      first_failure_time_(std::chrono::steady_clock::time_point::min()) {
    children_.reserve(children.size());
    for (auto& child : children) {
        children_.push_back(std::move(child));
    }
    become(make_behavior());
}

Behavior SupervisorActor::make_behavior() {
    return Behavior{[this](TypedMessage& msg) {
        if (msg.type_id() == TypeTag::DownMsg) {
            handle_child_down(msg.type_id(), msg.payload());
        }
    }};
}

void SupervisorActor::handle_child_down(TypeTag /*tag*/,
                                        const StreamBuffer& payload) {
    auto pb =
        mem::allocate_shared<::hpactor::DownMessage>(id(), mem::RegionType::kActor);
    if (!pb->ParseFromArray(payload.data(), static_cast<int>(payload.size()))) {
        return;
    }

    ActorId child_id(pb->actor_id());
    error reason(pb->reason_code());

    ChildFailure failure{child_id, reason, SupervisionDirective::Restart};
    auto directive = strategy_.on_child_failure(failure);

    switch (directive) {
        case SupervisionDirective::Restart:
            restart_child(child_id, reason);
            break;
        case SupervisionDirective::Stop:
            children_.erase(std::remove_if(children_.begin(), children_.end(),
                                           [&child_id](const Actor& a) {
                                               return a.id() == child_id;
                                           }),
                            children_.end());
            break;
        case SupervisionDirective::Escalate:
            break;
        case SupervisionDirective::Quarantine:
            // Child is transitioned to kQuarantined; remove from active
            // children list but keep in registry for inspection.
            children_.erase(std::remove_if(children_.begin(), children_.end(),
                                           [&child_id](const Actor& a) {
                                               return a.id() == child_id;
                                           }),
                            children_.end());
            break;
    }
}

void SupervisorActor::restart_child(ActorId child_id, const error& reason) {
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

    // Drive lifecycle for the failing child
    if (auto actor = system().get_actor(child_id)) {
        if (auto* lc = actor->as_lifecycle()) {
            lc->set_failure_reason(reason);
            lc->transition(LifecycleState::kFailed);
            lc->bump_incarnation();
            lc->transition(LifecycleState::kStarting);
        }
    }

    if (metrics_ring_buffer_) [[unlikely]] {
        metrics::MetricEvent evt{};
        evt.actor_id = id();
        evt.event_type = metrics::MetricEventType::kSupervisorRestart;
        evt.value_hi = static_cast<uint32_t>(child_id.value());
        metrics_ring_buffer_->try_push(evt);
    }
}

void SupervisorActor::restart_all_children() {
    for (auto& child : children_) {
        restart_child(child.id(), error(0));
    }
}

SelfSupervisingActor::SelfSupervisingActor(ActorContext* ctx, ActorSystem& sys,
                                           SupervisionPolicy policy)
    : EventBasedActor(ctx, sys), policy_(policy),
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
        if (child_addr == addr)
            return true;
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
            remote_children_.erase(remote_children_.begin() +
                                   static_cast<std::ptrdiff_t>(i));
            remote_child_addresses_.erase(remote_child_addresses_.begin() +
                                          static_cast<std::ptrdiff_t>(i));
            return;
        }
    }
}

SupervisionDirective
SelfSupervisingActor::on_failure(ActorId child_id, const error& err) {
    return decide_restart(child_id, err);
}

void SelfSupervisingActor::handle_child_down(TypeTag /*tag*/,
                                             const StreamBuffer& payload) {
    auto pb =
        mem::allocate_shared<::hpactor::DownMessage>(id(), mem::RegionType::kActor);
    if (!pb->ParseFromArray(payload.data(), static_cast<int>(payload.size()))) {
        return;
    }
    decide_restart(ActorId(pb->actor_id()), error(pb->reason_code()));
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
