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
    PythonBridgeActor(ActorContext* context, ActorSystem& system,
                      PythonRuntime& runtime, PythonActorLease lease,
                      PythonReliabilityController& reliability,
                      PythonSupervisionConfig supervision = {}) noexcept;

    void receive(TypedMessage& message) override;
    void on_activate() override;
    void on_deactivate() override;

    // ── LifecycleActor overrides ────────────────────────────────────────
    LifecycleActor* as_lifecycle() noexcept override {
        return this;
    }
    void on_drain() override;
    void on_stop() override;
    void on_fail(error err) override;
    void on_restart() override;
    void on_quarantined(QuarantineReason reason) override;

    [[nodiscard]] uint64_t generation() const noexcept;
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
