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

#include <hpactor/actor/abstract_actor.hpp>
#include <hpactor/actor/actor_route.hpp>
#include <hpactor/actor/lifecycle_actor.hpp>
#include <hpactor/actor/local_actor.hpp>

#include <sstream>

namespace hpactor {

// ── LocalActiveRoute ──────────────────────────────────────────

LocalActiveRoute::LocalActiveRoute(LocalActor* actor) : actor_(actor) {}

LocalActiveRoute::~LocalActiveRoute() = default;

LifecycleState LocalActiveRoute::state() const {
    auto* lc = actor_->as_lifecycle();
    return lc ? lc->state() : LifecycleState::kStopped;
}

bool LocalActiveRoute::is_active() const {
    return state() == LifecycleState::kActive;
}

std::string LocalActiveRoute::describe() const {
    std::ostringstream oss;
    oss << "LocalActiveRoute(actor_id=" << actor_->id().value() << ")";
    return oss.str();
}

ActorId LocalActiveRoute::actor_id() const {
    return actor_->id();
}

// ── LocalPassivatedRoute ──────────────────────────────────────

LocalPassivatedRoute::LocalPassivatedRoute(ActorId id, std::string persistence_id,
                                           PassivationRecord record,
                                           uint32_t buffer_capacity)
    : actor_id_(id), persistence_id_(std::move(persistence_id)),
      record_(std::move(record)), buffer_capacity_(buffer_capacity) {}

LocalPassivatedRoute::~LocalPassivatedRoute() = default;

LifecycleState LocalPassivatedRoute::state() const {
    if (reactivation_in_progress_.load(std::memory_order_acquire)) {
        return LifecycleState::kRecovering;
    }
    return static_cast<LifecycleState>(
        lifecycle_state_.load(std::memory_order_acquire));
}

bool LocalPassivatedRoute::is_active() const {
    return false;
}

std::string LocalPassivatedRoute::describe() const {
    std::ostringstream oss;
    oss << "LocalPassivatedRoute(actor_id=" << actor_id_.value()
        << ", persistence_id=" << persistence_id_ << ")";
    return oss.str();
}

ActorId LocalPassivatedRoute::actor_id() const {
    return actor_id_;
}

bool LocalPassivatedRoute::claim_reactivation() {
    bool expected = false;
    return reactivation_in_progress_.compare_exchange_strong(
        expected, true, std::memory_order_acq_rel, std::memory_order_acquire);
}

bool LocalPassivatedRoute::try_buffer_message() {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    if (buffer_count_ >= buffer_capacity_) {
        return false;
    }
    buffer_count_++;
    return true;
}

uint32_t LocalPassivatedRoute::buffered_count() const {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    return buffer_count_;
}

void LocalPassivatedRoute::transition_to_recovering() {
    uint8_t expected = static_cast<uint8_t>(LifecycleState::kPassivated);
    uint8_t desired = static_cast<uint8_t>(LifecycleState::kRecovering);
    lifecycle_state_.compare_exchange_strong(
        expected, desired, std::memory_order_acq_rel, std::memory_order_acquire);
}

void LocalPassivatedRoute::set_state(LifecycleState s) {
    lifecycle_state_.store(static_cast<uint8_t>(s), std::memory_order_release);
    // Clear the reactivation flag when reaching a terminal state
    if (s == LifecycleState::kActive || s == LifecycleState::kFailed ||
        s == LifecycleState::kStopped) {
        reactivation_in_progress_.store(false, std::memory_order_release);
    }
}

} // namespace hpactor
