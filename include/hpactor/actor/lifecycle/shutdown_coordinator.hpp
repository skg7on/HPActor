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
#include <hpactor/actor/lifecycle/shutdown_options.hpp>
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
    /// \brief Shared atomic running flag.
    std::atomic<bool>* running{nullptr};
    /// \brief Set the system readiness flag.
    std::function<void(bool)> set_ready;
    /// \brief Snapshot all live actor IDs with system/non-system
    /// classification.
    std::function<std::vector<std::pair<ActorId, bool>>()> actor_snapshot;
    /// \brief Look up an actor instance by ID.
    std::function<std::shared_ptr<class AbstractActor>(ActorId)> get_actor;
    /// \brief Look up an actor's mailbox by ID (returns void* to avoid
    ///        template instantiation; callee casts back).
    std::function<void*(ActorId)> get_mailbox_raw;
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
class ShutdownCoordinator {
  public:
    explicit ShutdownCoordinator(ShutdownCoordinatorDependencies deps);

    /// \brief Execute the full shutdown sequence with phase timeouts and
    ///        force-stop behaviour.
    void execute(const ShutdownOptions& opts);

    ShutdownPhase phase() const noexcept;
    bool accepting_ingress() const noexcept;

  private:
    void set_phase(ShutdownPhase phase) noexcept;
    void initiate_actor_drain(ActorId id);
    void poll_drain_complete(ActorId id,
                             std::chrono::steady_clock::time_point deadline);

    ShutdownCoordinatorDependencies deps_;
};

} // namespace hpactor
