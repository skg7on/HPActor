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

    // ── Observability runtime (construction only, no start yet) ────────────
    {
        ObservabilityRuntimeConfig obs_cfg;
        obs_cfg.metrics_enabled = bp.observability().metrics_enabled;
        obs_cfg.metrics_ring_buffer_capacity =
            bp.observability().metrics_ring_buffer_capacity;
        obs_cfg.logging_enabled = bp.observability().logging_enabled;
        obs_cfg.logging_ring_buffer_capacity =
            bp.observability().logging_ring_buffer_capacity;
        obs_cfg.tracing_enabled = bp.observability().tracing_enabled;
        obs_cfg.tracing_ring_buffer_capacity =
            bp.observability().tracing_ring_buffer_capacity;
        obs_cfg.fault_injection_enabled = bp.observability().fault_injection_enabled;

        observability_ = ObservabilityRuntime::create(obs_cfg);
    }

    // Start observability so ring buffer is available for messaging.
    (void)observability_->start();

    // ── Bind fixed network-control ports ────────────────────────────────────
    // Context points to this Impl; transport is reached via
    // impl->network_->transport().  Adapters check for null transport at
    // call time.
    messaging_ports.reliable_ack = ReliableAckEmitter{
        .context = this,
        .emit = Impl::reliable_ack_adapter,
    };
    messaging_ports.backpressure = BackpressureSignalEmitter{
        .context = this,
        .send = Impl::backpressure_wire_adapter,
    };

    // ── Messaging runtime ───────────────────────────────────────────────────
    // MUST be created BEFORE the scheduler because HybridScheduler's
    // ActorExecutionDependencies::from() accesses messaging_->dead_letters().
    messaging_ = std::make_unique<MessagingRuntime>(
        MessagingRuntime::Dependencies{
            .actors = actors.directory,
            .metrics = observability_->metrics_ring_buffer(),
            .network = messaging_ports,
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

    if (auto* ring = observability_->metrics_ring_buffer()) {
        core.scheduler->set_metrics_ring_buffer(ring);
    }

    // ── Fallback RpcChannel ──────────────────────────────────────────────────
    // Created unconditionally so rpc_channel() never returns a dangling
    // reference when networking is disabled.
    rpc_channel_ = std::make_unique<RpcChannel>(nullptr, core.scheduler.get(), 3);

    // ── Actor services ──────────────────────────────────────────────────────
    actors.type_registry = std::make_unique<ActorTypeRegistry>();

    // Spawner — constructed after directory, scheduler, metrics exist.
    spawner.emplace(ActorSpawner::Dependencies{
        .facade = f,
        .endpoint = core.endpoint,
        .directory = actors.directory,
        .scheduler = *core.scheduler,
        .metrics = observability_->metrics_ring_buffer(),
        .logger = observability_->logger(),
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
    observability_->fault_controller().install();
}

ActorSystem::Impl::~Impl() = default;

} // namespace hpactor
