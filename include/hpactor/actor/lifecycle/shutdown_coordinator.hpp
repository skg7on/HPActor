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

#include <atomic>
#include <chrono>
#include <functional>
#include <hpactor/actor/lifecycle/shutdown_phase.hpp>
#include <hpactor/types/types.hpp>

#include <memory>
#include <vector>

namespace hpactor {

/// \brief Callback bundle injected into \c ShutdownCoordinator by \c
/// ActorSystem.
///
/// Each function pointer bridges the coordinator to the owning system's
/// internals without exposing those internals through the header.
/// All fields are optional; the coordinator skips null callbacks.
struct ShutdownCoordinatorDependencies {
    /// \brief Shared atomic phase that the coordinator advances.
    std::atomic<ShutdownPhase>* phase{nullptr};
    /// \brief Set the system readiness flag.
    std::function<void(bool)> set_ready;
    /// \brief Snapshot all live actor IDs for drain.
    std::function<std::vector<ActorId>()> actor_snapshot;
    /// \brief Request a specific actor begin draining.
    std::function<void(ActorId)> request_actor_drain;
    /// \brief Returns \c true when all actors have completed draining.
    std::function<bool()> actors_drained;
    /// \brief Stop the remote runtime (RPC, transport accept loop).
    std::function<void()> stop_remote_runtime;
    /// \brief Leave the service discovery group.
    std::function<void()> leave_discovery;
    /// \brief Flush metrics, logs, and traces before final stop.
    std::function<void()> flush_telemetry;
};

/// \brief Drives the node shutdown phase machine.
///
/// Receives dependency callbacks from \c ActorSystem and advances through
/// each \c ShutdownPhase in order: ingress drain, actor drain, cluster
/// leave, telemetry flush, and stop.
///
/// \note Thread safety: \c phase() and \c accepting_ingress() are safe to
///       call from any thread. \c shutdown() should be called from a single
///       control thread.
class ShutdownCoordinator {
  public:
    /// \brief Construct with injected dependency callbacks.
    ///
    /// \param[in] deps Callback bundle. Each callback may be null; the
    ///                 coordinator skips null entries during shutdown.
    explicit ShutdownCoordinator(ShutdownCoordinatorDependencies deps);

    /// \brief Execute the full shutdown sequence.
    ///
    /// Advances through DrainingIngress, DrainingActors, LeavingCluster,
    /// FlushingTelemetry, and Stopped phases in order.
    /// \param[in] drain_timeout Maximum time to wait for actor drain.
    void shutdown(std::chrono::milliseconds drain_timeout);

    /// \brief Current shutdown phase.
    ///
    /// \return The phase last written by the coordinator, or
    ///         \c ShutdownPhase::Running if the phase pointer is null.
    ShutdownPhase phase() const noexcept;

    /// \brief Returns \c true only while the system is in the Running phase.
    ///
    /// Used to gate ingress acceptance at network and spawn boundaries.
    bool accepting_ingress() const noexcept;

  private:
    void set_phase(ShutdownPhase phase) noexcept;
    ShutdownCoordinatorDependencies deps_;
};

} // namespace hpactor
