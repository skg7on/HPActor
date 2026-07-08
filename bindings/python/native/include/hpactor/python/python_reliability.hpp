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

#include <hpactor/python/python_bridge_types.hpp>
#include <hpactor/ref/actor_address.hpp>
#include <hpactor/types/types.hpp>

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

namespace hpactor::python {

/// \brief Decision a reliability controller makes after a failure.
enum class PythonFailureDirective : uint8_t {
    Restart = 0,    ///< Allocate replacement generation and reconstruct.
    Stop = 1,       ///< Stop the actor permanently.
    Escalate = 2,   ///< Escalate to the parent supervisor.
    Quarantine = 3, ///< Stop and quarantine the actor.
};

/// \brief Per-actor supervision policy for Python actors.
struct PythonSupervisionConfig final {
    uint32_t max_restarts{10};
    uint32_t restart_window_ms{5000};
    bool quarantine_on_exhaustion{false};
};

/// \brief Fixed function-pointer port for reliability callbacks.
struct PythonReliabilityPort final {
    void* context{nullptr};
    void (*on_failure)(void*, const ActorAddress&, uint64_t,
                       const PythonFailureMetadata&) noexcept {nullptr};
    void (*on_restart_ready)(void*, const ActorAddress&,
                             uint64_t) noexcept {nullptr};
};

/// \brief Tracks per-actor reliability state: generation, policy, restart
///        budget, and makes directive decisions. Does not call Python.
class PythonReliabilityController final {
  public:
    /// \brief Construct with optional reliability port.
    explicit PythonReliabilityController(PythonReliabilityPort port = {}) noexcept;

    /// \brief Record a failure and return the directive.
    ///
    /// \param[in] actor The actor that failed.
    /// \param[in] generation The generation that failed.
    /// \param[in] metadata Bounded failure metadata.
    /// \param[in] now_ms Current monotonic timestamp in milliseconds.
    /// \return The directive to apply.
    PythonFailureDirective
    record_failure(const ActorAddress& actor, uint64_t generation,
                   const PythonFailureMetadata& metadata, uint64_t now_ms) noexcept;

    /// \brief Register an actor for tracking with the given policy.
    ///
    /// \param[in] actor The actor address.
    /// \param[in] generation The initial generation.
    /// \param[in] policy The supervision policy.
    void register_actor(const ActorAddress& actor, uint64_t generation,
                        PythonSupervisionConfig policy) noexcept;

    /// \brief Unregister an actor (e.g., on clean stop).
    void unregister_actor(const ActorAddress& actor) noexcept;

    /// \brief Advance the tracked generation for a restarting actor.
    void advance_generation(const ActorAddress& actor,
                            uint64_t new_generation) noexcept;

    /// \brief Get the supervision config for an actor.
    PythonSupervisionConfig
    supervision_config(const ActorAddress& actor) const noexcept;

  private:
    struct ActorReliabilityState {
        uint64_t generation{0};
        PythonSupervisionConfig policy;
        uint32_t restart_count{0};
        uint64_t window_start_ms{0};
        bool quarantined{false};
    };

    PythonReliabilityPort port_;
    mutable std::mutex mutex_;
    std::unordered_map<ActorId, ActorReliabilityState> actors_;
};

} // namespace hpactor::python
