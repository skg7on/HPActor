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

#include "runtime_coordinator.hpp"

namespace hpactor {

void RuntimeCoordinator::add_stage(RuntimeLifecycleStage stage) {
    stages_.push_back(stage);
}

void RuntimeCoordinator::transition_to(RuntimeLifecycleState new_state) noexcept {
    state_.store(new_state, std::memory_order_release);
    epoch_.fetch_add(1, std::memory_order_release);
    // Readiness is only true in Running state.
    ready_.store(new_state == RuntimeLifecycleState::Running,
                 std::memory_order_release);
}

RuntimeLifecycleState RuntimeCoordinator::state() const noexcept {
    return state_.load(std::memory_order_acquire);
}

bool RuntimeCoordinator::is_ready() const noexcept {
    return ready_.load(std::memory_order_acquire);
}

RuntimeLifecycleSnapshot RuntimeCoordinator::snapshot() const noexcept {
    RuntimeLifecycleSnapshot snap;
    snap.state = state();
    snap.ready = is_ready();
    snap.transition_epoch = epoch_.load(std::memory_order_acquire);
    snap.last_stage = last_stage_;
    snap.primary_error_code = primary_error_.load(std::memory_order_acquire);
    snap.rollback_error_bits = rollback_bits_.load(std::memory_order_acquire);
    return snap;
}

void RuntimeCoordinator::rollback(size_t completed_count) noexcept {
    // Rollback in reverse order, skipping the failed stage.
    for (size_t i = completed_count; i > 0; --i) {
        const auto& stage = stages_[i - 1];
        if (stage.rollback.action) {
            stage.rollback.action(stage.rollback.context);
        }
    }
}

result<void> RuntimeCoordinator::start() noexcept {
    auto current = state_.load(std::memory_order_acquire);

    // Can only start from Built or Failed state.
    if (current != RuntimeLifecycleState::Built &&
        current != RuntimeLifecycleState::Failed) {
        return result<void>::make(
            error(errors::invalid_argument, "coordinator already started"));
    }

    transition_to(RuntimeLifecycleState::Starting);

    for (size_t i = 0; i < stages_.size(); ++i) {
        last_stage_ = stages_[i].name;

        bool ok = true;
        if (stages_[i].start.action) {
            ok = stages_[i].start.action(stages_[i].start.context);
        }

        if (!ok) {
            rollback(i); // rollback stages 0..i-1
            transition_to(RuntimeLifecycleState::Failed);
            primary_error_.store(1, std::memory_order_release);
            return result<void>::make(
                error(errors::actor_down, "startup stage failed"));
        }
    }

    transition_to(RuntimeLifecycleState::Running);
    return result<void>::make();
}

result<void> RuntimeCoordinator::stop() noexcept {
    auto current = state_.load(std::memory_order_acquire);

    // Already stopped — idempotent.
    if (current == RuntimeLifecycleState::Stopped) {
        return result<void>::make();
    }

    // If running, transition through drain→stop.
    if (current == RuntimeLifecycleState::Running) {
        transition_to(RuntimeLifecycleState::Draining);
        transition_to(RuntimeLifecycleState::Stopping);
    } else {
        // From any other state (Built, Failed, etc.), go directly to Stopped.
        transition_to(RuntimeLifecycleState::Stopping);
    }

    transition_to(RuntimeLifecycleState::Stopped);
    return result<void>::make();
}

} // namespace hpactor
