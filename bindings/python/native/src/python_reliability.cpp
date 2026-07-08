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

#include <hpactor/python/python_reliability.hpp>

#include <algorithm>

namespace hpactor::python {

PythonReliabilityController::PythonReliabilityController(PythonReliabilityPort port) noexcept
    : port_(port) {}

PythonFailureDirective
PythonReliabilityController::record_failure(const ActorAddress& actor,
                                            uint64_t generation,
                                            const PythonFailureMetadata& metadata,
                                            uint64_t now_ms) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = actors_.find(actor.id);
    if (it == actors_.end()) {
        return PythonFailureDirective::Stop;
    }

    auto& state = it->second;

    // Reset window if it expired.
    if (now_ms - state.window_start_ms > state.policy.restart_window_ms) {
        state.restart_count = 0;
        state.window_start_ms = now_ms;
    }

    ++state.restart_count;

    if (port_.on_failure && port_.context) {
        port_.on_failure(port_.context, actor, generation, metadata);
    }

    if (state.quarantined) {
        return PythonFailureDirective::Stop;
    }

    if (state.restart_count > state.policy.max_restarts) {
        if (state.policy.quarantine_on_exhaustion) {
            state.quarantined = true;
            return PythonFailureDirective::Quarantine;
        }
        return PythonFailureDirective::Stop;
    }

    return PythonFailureDirective::Restart;
}

void PythonReliabilityController::register_actor(const ActorAddress& actor,
                                                 uint64_t generation,
                                                 PythonSupervisionConfig policy) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    ActorReliabilityState state;
    state.generation = generation;
    state.policy = policy;
    state.window_start_ms = 0;
    actors_[actor.id] = state;
}

void PythonReliabilityController::unregister_actor(const ActorAddress& actor) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    actors_.erase(actor.id);
}

void PythonReliabilityController::advance_generation(const ActorAddress& actor,
                                                     uint64_t new_generation) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = actors_.find(actor.id);
    if (it != actors_.end()) {
        it->second.generation = new_generation;
    }
}

PythonSupervisionConfig
PythonReliabilityController::supervision_config(const ActorAddress& actor) const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = actors_.find(actor.id);
    if (it != actors_.end()) {
        return it->second.policy;
    }
    return PythonSupervisionConfig{};
}

} // namespace hpactor::python
