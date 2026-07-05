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

#include "actor_system_impl.hpp"

#include <hpactor/runtime/actor_spawner.hpp>
#include <hpactor/runtime/messaging_runtime.hpp>
#include <hpactor/runtime/runtime_blueprint.hpp>

#include <hpactor/actor/lifecycle/shutdown_coordinator.hpp>
#include <hpactor/actor/spawn/actor_type_registry.hpp>
#include <hpactor/fault/fault_controller.hpp>
#include <hpactor/metrics/metrics_config.hpp>
#include <hpactor/metrics/metrics_ring_buffer.hpp>
#include <hpactor/msg/proto_type_registry.hpp>
#include <hpactor/sched/scheduler.hpp>

namespace hpactor {

// ── Config-based constructor (delegates to ActorSystem constructor) ─────────

ActorSystem::Impl::Impl(ActorSystem& f, const Config& /*config*/) : facade(f) {}

// ── Blueprint-based constructor (construction only, no startup) ─────────

ActorSystem::Impl::Impl(ActorSystem& f, const RuntimeBlueprint& bp)
    : facade(f) {
    // ── Core runtime state (set BEFORE any component construction) ─────────
    core.endpoint = bp.actor().endpoint;
    core.start_time = std::chrono::steady_clock::now();
    core.proto_registry.register_system_types();
}

ActorSystem::Impl::~Impl() = default;

} // namespace hpactor
