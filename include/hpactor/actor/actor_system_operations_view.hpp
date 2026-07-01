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

#include <hpactor/actor/lifecycle/shutdown_phase.hpp>
#include <hpactor/observability/observability_snapshot.hpp>

namespace hpactor {

// Forward declarations — these are private runtime types.
class ObservabilityRuntime;
struct CoreRuntimeState;

/// \brief Non-owning read-only view of operations state.
///
/// Obtained from \c ActorSystem::operations().  Provides aggregate
/// lifecycle, health, and observability state for CLI, admin API,
/// and health endpoints.  Lifetime must not exceed the parent
/// \c ActorSystem.
///
/// \note All methods are thread-safe; snapshots are bounded copies.
class ActorSystemOperationsView final {
  public:
    /// \brief System uptime since construction.
    [[nodiscard]] std::chrono::milliseconds uptime() const noexcept;

    /// \brief Whether the system is currently running.
    [[nodiscard]] bool is_running() const noexcept;

    /// \brief Whether the system is ready to accept work.
    [[nodiscard]] bool is_ready() const noexcept;

    /// \brief Whether the system is in the draining phase.
    [[nodiscard]] bool is_draining() const noexcept;

    /// \brief Current shutdown phase.
    [[nodiscard]] ShutdownPhase shutdown_phase() const noexcept;

    /// \brief Aggregate snapshot of all runtime components.
    [[nodiscard]] OperationsSnapshot snapshot() const noexcept;

  private:
    friend class ActorSystem;

    ActorSystemOperationsView(const CoreRuntimeState& core,
                              const ObservabilityRuntime* observability,
                              size_t actor_count) noexcept;

    const CoreRuntimeState* core_;
    const ObservabilityRuntime* observability_;
    size_t actor_count_;
};

} // namespace hpactor
