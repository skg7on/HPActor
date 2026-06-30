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

#include "actor_spawner.hpp"
#include "messaging_runtime.hpp"
#include "runtime_blueprint.hpp" // private blueprint header

#include <hpactor/actor/actor_type_registry.hpp>
#include <hpactor/actor/lifecycle/shutdown_coordinator.hpp>
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
    // ── Core runtime state ──────────────────────────────────────────────────
    core.endpoint = bp.actor().endpoint;
    core.start_time = std::chrono::steady_clock::now();

    core.proto_registry.register_system_types();

    // ── Metrics ring buffer ─────────────────────────────────────────────────
    if (operations.metrics_config.enabled) {
        operations.metrics_ring_buffer =
            std::make_shared<metrics::MpscRingBuffer<metrics::MetricEvent>>();
    }

    // ── Bind fixed network-control ports ────────────────────────────────────
    // Context pointer uses legacy NetworkRuntimeState; adapters are wired
    // during start (Task 5).  Context is valid (fields exist, transport is
    // null).
    network.messaging_ports.reliable_ack = ReliableAckPort{
        .context = &network,
        .emit = nullptr, // wired during coordinator start
    };
    network.messaging_ports.backpressure = BackpressureWirePort{
        .context = &network,
        .send = nullptr, // wired during coordinator start
    };

    // ── Messaging runtime ───────────────────────────────────────────────────
    // MUST be created BEFORE the scheduler because HybridScheduler's
    // ActorExecutionDependencies::from() accesses messaging_->dead_letters().
    messaging_ = std::make_unique<MessagingRuntime>(
        MessagingRuntime::Dependencies{
            .actors = actors.directory,
            .metrics = operations.metrics_ring_buffer.get(),
            .network = network.messaging_ports,
            .endpoint = core.endpoint,
        },
        MessagingRuntime::Config{
            .dead_letters = {},
            .default_message_ttl = bp.messaging().default_message_ttl_ms,
        });

    // ── Scheduler ───────────────────────────────────────────────────────────
    // Create after messaging_ so ActorExecutionDependencies::from() can
    // resolve dead_letters().  Do NOT start the scheduler (no worker threads).
    core.scheduler = std::make_unique<sched::HybridScheduler>(
        f, bp.actor().scheduler_threads, 4 /* num_priorities */,
        sched::TimerBackend::TimingWheel, bp.actor().scheduler_start_paused);

    if (operations.metrics_ring_buffer) {
        core.scheduler->set_metrics_ring_buffer(
            operations.metrics_ring_buffer.get());
    }

    // ── Actor services ──────────────────────────────────────────────────────
    actors.type_registry = std::make_unique<ActorTypeRegistry>();

    // Spawner — constructed after directory, scheduler, metrics exist.
    spawner.emplace(ActorSpawner::Dependencies{
        .facade = f,
        .endpoint = core.endpoint,
        .directory = actors.directory,
        .scheduler = *core.scheduler,
        .metrics = operations.metrics_ring_buffer.get(),
        .logger = nullptr, // logger not yet created (start phase)
    });

    // ── Shutdown coordinator (placeholder — replaced by RuntimeCoordinator
    //    in Task 4) ───────────────────────────────────────────────────────────
    {
        ShutdownCoordinatorDependencies sc_deps;
        sc_deps.phase = &core.shutdown_phase;
        sc_deps.running = &core.running;
        actors.shutdown_coordinator =
            std::make_unique<ShutdownCoordinator>(sc_deps);
    }

    // ── Fault controller ────────────────────────────────────────────────────
    // Installed but no fault points are active by default.
    operations.fault_controller.install();
}

ActorSystem::Impl::~Impl() = default;

} // namespace hpactor
