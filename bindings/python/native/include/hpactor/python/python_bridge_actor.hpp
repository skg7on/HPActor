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

#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/actor/lifecycle/lifecycle_actor.hpp>
#include <hpactor/python/python_reliability.hpp>
#include <hpactor/python/python_runtime.hpp>

#include <cstdint>
#include <string_view>

namespace hpactor::python {

/// \brief Native C++ bridge actor that serves as the HPActor-side identity for
///        each Python-bound actor.
///
/// Phase 1C: inherits LifecycleActor for supervision and restart support.
/// Lifecycle hooks (on_fail, on_restart, on_quarantined, etc.) coordinate
/// with the PythonReliabilityController and PythonRuntime to fence stale
/// generations and dispatch restart/replacement events.
class PythonBridgeActor final : public EventBasedActor, public LifecycleActor {
  public:
    static constexpr std::string_view kActorTypeName{"hpactor.python.bridge"};

    /// \brief Construct the bridge actor.
    ///
    /// \param[in] context The actor's execution context. Must outlive the actor.
    /// \param[in] system The owning actor system. Must outlive the actor.
    /// \param[in] runtime The Python runtime for dispatch enqueue and lease
    ///                    management. Must outlive the actor.
    /// \param[in] lease The Python actor lease with monotonic generation.
    /// \param[in] reliability The reliability controller for failure tracking.
    ///                        Must outlive the actor.
    /// \param[in] supervision Per-actor supervision policy (max restarts,
    ///                        restart window, quarantine on exhaustion).
    PythonBridgeActor(ActorContext* context, ActorSystem& system,
                      PythonRuntime& runtime, PythonActorLease lease,
                      PythonReliabilityController& reliability,
                      PythonSupervisionConfig supervision = {}) noexcept;

    /// \brief Receive and dispatch an incoming typed message.
    ///
    /// Converts the HPActor TypedMessage into a PythonDispatchEnvelope
    /// and enqueues it onto the runtime's dispatch ring buffer.
    ///
    /// \param[in,out] message The typed message to dispatch.
    void receive(TypedMessage& message) override;

    /// \brief Called after the actor is fully constructed and bound to a
    ///        scheduler thread.
    void on_activate() override;

    /// \brief Called before the actor is torn down from the scheduler.
    void on_deactivate() override;

    // ── LifecycleActor overrides ────────────────────────────────────────

    /// \brief Return this actor as a LifecycleActor for supervision.
    ///
    /// \return Pointer to the LifecycleActor base.
    LifecycleActor* as_lifecycle() noexcept override {
        return this;
    }

    /// \brief Drain in-flight messages. Rejects new dispatches.
    void on_drain() override;

    /// \brief Stop the actor. Fences the generation lease.
    void on_stop() override;

    /// \brief Handle a failure with the given error.
    ///
    /// \param[in] err The error that triggered the failure.
    void on_fail(error err) override;

    /// \brief Allocate a replacement generation and re-enqueue for restart.
    ///
    /// Enqueues a PythonDispatchKind::Restart dispatch so the Python runtime
    /// can reconstruct the actor's handler and state.
    void on_restart() override;

    /// \brief Quarantine the actor with the given reason.
    ///
    /// \param[in] reason Why the actor was quarantined (repeated failures,
    ///                   supervision policy exhaustion, etc.).
    void on_quarantined(QuarantineReason reason) override;

    /// \brief The current monotonic generation of this actor.
    ///
    /// \return The lease generation, incremented on each restart.
    [[nodiscard]] uint64_t generation() const noexcept;

    /// \brief The actor type name for metrics and logging.
    ///
    /// \return "hpactor.python.bridge".
    std::string_view type_name() const noexcept override {
        return kActorTypeName;
    }

  private:
    PythonRuntime& runtime_;
    PythonActorLease lease_;
    PythonReliabilityController& reliability_;
    PythonSupervisionConfig supervision_;
    uint64_t next_dispatch_sequence_{1};
};

} // namespace hpactor::python
