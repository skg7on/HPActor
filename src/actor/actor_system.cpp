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

#include <hpactor/actor/actor_system.hpp>
#include <hpactor/actor/actor_type_registry.hpp>

#include "../runtime/actor_spawner.hpp"
#include "../runtime/actor_system_impl.hpp"
#include "../runtime/runtime_blueprint.hpp"
#include "../runtime/runtime_blueprint_builder.hpp"
#include "../runtime/runtime_builder.hpp"
#include "../runtime/runtime_coordinator.hpp"
#include "../runtime/runtime_startup.hpp"
#include <hpactor/actor/ask_manager.hpp>
#include <hpactor/actor/durable/in_memory_state_store.hpp>
#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/actor/http_gateway_actor.hpp>
#include <hpactor/actor/lifecycle/passivation_config.hpp>
#include <hpactor/actor/lifecycle/passivation_manager.hpp>
#include <hpactor/actor/lifecycle/shutdown_coordinator.hpp>
#include <hpactor/actor/local_actor.hpp>
#include <hpactor/actor/spawn_receiver.hpp>
#include <hpactor/actor/stream_receiver_actor.hpp>
#include <hpactor/actor/stream_sender_actor.hpp>
#include <hpactor/actor/stream_types.hpp>
#include <hpactor/config/actor_factory_registry.hpp>
#include <hpactor/config/toml_parser.hpp>
#include <hpactor/fault/fault_macros.hpp>
#include <hpactor/hpactor_config.hpp>
#include <hpactor/mailbox/backpressure_coordinator.hpp>
#include <hpactor/mailbox/delivery_pipeline.hpp>
#include <hpactor/mailbox/local_delivery_engine.hpp>
#include <hpactor/mailbox/outbound_tracker.hpp>
#include <hpactor/mailbox/reliable_retry_policy.hpp>
#include <hpactor/msg/outbound_delivery_tracker.hpp>
#include <hpactor/process/process_manager.hpp>

#include <chrono>
#include <mutex>
#include <thread>

#include <hpactor/actor/receptionist/receptionist.hpp>
#include <hpactor/actor/spawn.hpp>
#include <hpactor/cli/actor/cli_local_actor.hpp>
#include <hpactor/log/log_manager.hpp>
#include <hpactor/log/logger.hpp>
#include <hpactor/mailbox/backpressure_signal_serialization.hpp>
#include <hpactor/mem/std_allocator.hpp>
#include <hpactor/metrics/metrics_actor.hpp>
#include <hpactor/msg/failure_envelope.hpp>
#include <hpactor/msg/frame.hpp>
#include <hpactor/net/async_io_fwd.hpp>
#include <hpactor/net/tcp_transport.hpp>
#include <hpactor/sched/scheduler.hpp>
#include <hpactor/types/types.hpp>

#include <hpactor/common.pb.h>
#include <hpactor/messages.pb.h>

namespace hpactor {

namespace {

// ── Network port adapter functions ───────────────────────────────────────
//
// These are bound to ReliableAckPort and BackpressureWirePort function
// pointers.  The void* context points to a stable NetworkRuntimeState
// instance.  Neither function captures ActorSystem or Impl.

void reliable_ack_adapter(void* context, const ActorAddress& target,
                          const ActorAddress& acker, uint64_t message_id,
                          uint8_t status, uint32_t retry_after_ms) noexcept {
    auto* net = static_cast<NetworkRuntimeState*>(context);
    if (!net || !net->transport)
        return;

    net::WireFrame frame;
    bool is_nack = (status == 1); // 1 = AckStatus::Rejected
    frame.pb_envelope.mutable_data_frame()->set_flags(
        is_nack ? net::WireFrame::AckResponse : net::WireFrame::AckRequested);
    frame.pb_envelope.mutable_data_frame()->set_message_id(message_id);
    net::to_proto(frame.pb_envelope.mutable_data_frame()->mutable_sender(), acker);
    net::to_proto(frame.pb_envelope.mutable_data_frame()->mutable_receiver(),
                  target);

    if (is_nack) {
        frame.pb_envelope.mutable_data_frame()->set_type_tag(
            static_cast<uint32_t>(status));
        std::string payload_str(reinterpret_cast<const char*>(&retry_after_ms),
                                sizeof(uint32_t));
        frame.pb_envelope.mutable_data_frame()->set_payload(payload_str);
    }

    auto encoded = frame.encode();
    (void)net->transport->try_send(target, encoded);
}

bool backpressure_wire_adapter(void* context, const ActorAddress& target,
                               const StreamBuffer& encoded) noexcept {
    auto* net = static_cast<NetworkRuntimeState*>(context);
    if (!net)
        return false;

    if (net->backpressure_signal_wire_sink_for_test) {
        return net->backpressure_signal_wire_sink_for_test(target, encoded);
    }
    if (net->transport) {
        return net->transport->try_send(target, encoded) ==
               TransportSendResult::Sent;
    }
    return false;
}

} // namespace

// -----------------------------------------------------------------------------
// ActorSystem implementation
// -----------------------------------------------------------------------------

// ── Blueprint-based construction (no startup) ───────────────────────────────
// Only accessible to RuntimeBuilder and RuntimeCoordinator (friends).
ActorSystem::ActorSystem(FromBlueprint, const RuntimeBlueprint& bp)
    : impl_(std::make_unique<Impl>(*this, bp)) {
    // The Impl blueprint constructor builds all components but does NOT start
    // threads, listeners, timers, or spawn actors.  The RuntimeCoordinator
    // owns startup ordering (Task 5).
}

// ── Preferred factory: blueprint → builder → coordinator → ready ──────────

result<std::unique_ptr<ActorSystem>>
ActorSystem::create(const Config& config) noexcept {
    auto bp = RuntimeBlueprintBuilder::from_config(config);
    if (!bp.ok()) {
        return result<std::unique_ptr<ActorSystem>>::make(bp.error());
    }

    auto built = RuntimeBuilder::build(bp.value());
    if (!built.ok()) {
        return result<std::unique_ptr<ActorSystem>>::make(built.error());
    }

    // RuntimeBuilder already used the FromBlueprint constructor which
    // does NOT start threads.  The system is valid but not ready.
    auto system = std::move(built.value().system);

    // If the user wants an immediately-ready system (legacy behavior),
    // we need to start the scheduler.  The existing Config constructor
    // path does this; the blueprint path defers to the coordinator.
    // For source compatibility, start the scheduler now.
    if (system->scheduler()) {
        system->scheduler()->start();
    }

    return result<std::unique_ptr<ActorSystem>>::make(std::move(system));
}

result<std::unique_ptr<ActorSystem>>
ActorSystem::create(const Config& config, const std::string& topology_path) noexcept {
    auto sys_result = create(config);
    if (!sys_result.ok()) {
        return sys_result;
    }
    auto load_result = sys_result.value()->load_topology(topology_path);
    if (!load_result.ok()) {
        return result<std::unique_ptr<ActorSystem>>::make(load_result.error());
    }
    return sys_result;
}

// ── Config-based construction (full startup, legacy path) ───────────────────
ActorSystem::ActorSystem(const Config& config)
    : impl_(std::make_unique<Impl>(*this, config)) {
    impl_->core.config = config;
    impl_->core.endpoint = config.endpoint;
    impl_->core.start_time = std::chrono::steady_clock::now();
    impl_->core.scheduler = std::make_unique<sched::HybridScheduler>(
        *this, config.scheduler_threads, 4, config.timer_backend,
        config.scheduler_start_paused);
    impl_->actors.type_registry = std::make_unique<ActorTypeRegistry>();

    // ── Spawner ──────────────────────────────────────────────────────────
    // Must exist before any spawn<>() call.
    // Metrics and logger pointers are captured by address so later
    // assignments to the shared_ptr/pointer are visible to the spawner.
    impl_->spawner.emplace(ActorSpawner::Dependencies{
        .facade = *this,
        .endpoint = impl_->core.endpoint,
        .directory = impl_->actors.directory,
        .scheduler = *impl_->core.scheduler,
        .metrics = nullptr, // set after metrics ring buffer is created
        .logger = nullptr,  // set after logger is created
    });
    // ── System protobuf types ──────────────────────────────────────────
    impl_->core.proto_registry.register_system_types();

    // ── Observability runtime ────────────────────────────────────────────
    // Create and start before any producer so stable ports are available.
    {
        ObservabilityRuntimeConfig obs_cfg;
        obs_cfg.metrics_enabled = impl_->operations.metrics_config.enabled;
        obs_cfg.metrics_ring_buffer_capacity =
            impl_->operations.metrics_config.ring_buffer_capacity;
        obs_cfg.logging_enabled = impl_->operations.logging_config.enabled;
        obs_cfg.logging_ring_buffer_capacity =
            impl_->operations.logging_config.ring_buffer_capacity;
        obs_cfg.tracing_enabled = impl_->core.config.tracing.enabled;
        obs_cfg.tracing_ring_buffer_capacity =
            impl_->core.config.tracing.ring_buffer_capacity;
        obs_cfg.fault_injection_enabled = true;

        impl_->observability_ = ObservabilityRuntime::create(obs_cfg);
        (void)impl_->observability_->start();

        // Wire scheduler to observability ports.
        if (impl_->observability_->metrics_ring_buffer()) {
            impl_->core.scheduler->set_metrics_ring_buffer(
                impl_->observability_->metrics_ring_buffer());
        }
    }

    // ── Bind fixed network-control ports ────────────────────────────────
    // Context points to stable NetworkRuntimeState; transport is still null
    // but will become valid when networking is initialized later.
    {
        impl_->network.messaging_ports.reliable_ack = ReliableAckPort{
            .context = &impl_->network,
            .emit = reliable_ack_adapter,
        };
        impl_->network.messaging_ports.backpressure = BackpressureWirePort{
            .context = &impl_->network,
            .send = backpressure_wire_adapter,
        };
    }

    // ── Messaging runtime ───────────────────────────────────────────────
    // Constructs all seven messaging components in dependency order
    // (DLQ → Dedup → Trackers → Backpressure → Pipeline → FastEngine).
    // MUST be created before the scheduler starts so that early-boot
    // actor spawns can deliver messages through the pipeline.
    impl_->messaging_ = std::make_unique<MessagingRuntime>(
        MessagingRuntime::Dependencies{
            .actors = impl_->actors.directory,
            .metrics = impl_->observability_->metrics_ring_buffer(),
            .network = impl_->network.messaging_ports,
            .endpoint = impl_->core.endpoint,
        },
        MessagingRuntime::Config{
            .dead_letters = impl_->core.config.dead_letters,
            .default_message_ttl = impl_->core.config.default_message_ttl_ms,
        });

    // ── Metrics actor (owned by ActorRuntime, not ObservabilityRuntime) ──
    if (impl_->observability_->metrics_config().enabled) {
        auto m_actor = spawn<metrics::MetricsActor>(
            impl_->observability_->metrics_ring_buffer_shared());
        impl_->operations.metrics_actor =
            static_cast<metrics::MetricsActor*>(m_actor.get().get());
    }

    // ── Logger wiring to scheduler ──────────────────────────────────────
    if (impl_->observability_->logger()) [[unlikely]] {
        impl_->core.scheduler->set_logger(impl_->observability_->logger());
    }

    // Reconstruct spawner with now-valid metrics/logger pointers.
    impl_->spawner.emplace(ActorSpawner::Dependencies{
        .facade = *this,
        .endpoint = impl_->core.endpoint,
        .directory = impl_->actors.directory,
        .scheduler = *impl_->core.scheduler,
        .metrics = impl_->observability_->metrics_ring_buffer(),
        .logger = impl_->observability_->logger(),
    });

    impl_->observability_->fault_controller().set_log_manager(
        impl_->observability_->log_manager());

    // Initialize process-mode subystem (daemonize, pidfile, signal handling).
    // Must happen before impl_->core.scheduler->start() so daemonization forks
    // before any worker threads are created.
    (void)process::ProcessManager::init(impl_->core.config.process);

    impl_->core.scheduler->start();

    impl_->observability_->apply_tracing_config(impl_->core.config.tracing);

    if (config.enable_network) {
        // ── Build NetworkRuntimeConfig from facade Config ────────────────
        NetworkRuntimeConfig net_config;
        net_config.enabled = true;
        net_config.endpoint = impl_->core.endpoint;
        net_config.tcp_port = config.tcp_port;
        net_config.tls = config.tls;
        net_config.pool = config.pool;
        net_config.service_discovery = config.service_discovery;
        net_config.registrar = config.registrar;
        net_config.enable_http_client = config.enable_http_client;
        net_config.enable_http_gateway = config.enable_http_gateway;
        net_config.http_bind_host = config.http_bind_host;
        net_config.http_port = config.http_port;
        net_config.ask_max_retries = config.default_ask_max_retries;

        // ── Build fixed dependencies ─────────────────────────────────────
        NetworkRuntime::Dependencies net_deps;
        net_deps.actors = &impl_->actors.directory;
        net_deps.messaging = impl_->messaging_.get();
        net_deps.scheduler = impl_->core.scheduler.get();

        // Inbound frame sink — route remote frames into deliver_remote.
        net_deps.inbound_sink = InboundFrameSinkPort{
            .context = this,
            .sink =
                [](void* ctx, const net::WireFrame& frame) noexcept {
                    static_cast<ActorSystem*>(ctx)->deliver_remote(frame);
                },
        };

        // Node event sink — route member changes to on_node_dead.
        net_deps.node_events = NodeEventSink{
            .context = this,
            .member_changed =
                [](void* ctx, const net::Member& m, bool joined) noexcept {
                    if (!joined) {
                        static_cast<ActorSystem*>(ctx)->on_node_dead(
                            m.identity.endpoint);
                    }
                },
        };

        // Retry port — route to MessagingRuntime::process_retries.
        net_deps.retry_port = OutboundRetryPort{
            .context = impl_->messaging_.get(),
            .process_due =
                [](void* ctx, uint64_t now_ns) noexcept {
                    static_cast<MessagingRuntime*>(ctx)->process_retries(
                        now_ns,
                        [](const msg::OutboundDeliveryTracker::PendingSend&) {
                            // Resend via transport (follow-up).
                        });
                },
        };

        // Spawn port — canonical actor adoption through spawner.
        // Phase 6 moves SpawnReceiver into ActorRuntime fully.
        net_deps.spawn_port = RemoteSpawnPort{
            .context = this,
            .install_receiver = nullptr, // wired in Phase 6
            .remove_receiver = nullptr,  // wired in Phase 6
        };

        // Create and start the network runtime.
        impl_->network_ = std::make_unique<NetworkRuntime>(net_config, net_deps);

        // Transfer metrics ring buffer pointer to transport.
        if (auto* ring = impl_->observability_->metrics_ring_buffer()) {
            if (auto* t = impl_->network_->transport()) {
                t->set_metrics_ring_buffer(ring);
            }
        }

        // Start networking.
        auto start_result = impl_->network_->start();
        if (start_result.is_error()) {
            impl_->core.running.store(false, std::memory_order_release);
            // start() already rolled back on failure.
        }

        // ── Actor services that depend on network being started ──────────
        impl_->actors.ask =
            std::make_unique<AskManager>(impl_->core.scheduler.get(), this);

        if (impl_->core.config.enable_http_gateway) {
            impl_->actors.http_gateway_actor = spawn<net::HTTPGatewayActor>(
                impl_->core.config.http_bind_host, impl_->core.config.http_port);
        }

        // ── Manual SpawnReceiver setup (TODO: move to ActorRuntime in
        //    Phase 6, using RemoteSpawnPort properly) ──────────────────────
        auto spawn_receiver = std::make_shared<SpawnReceiver>(
            *this, *impl_->actors.type_registry, impl_->network_->transport());

        SpawnSpec receiver_spec;
        receiver_spec.type_name = "SpawnReceiver";
        receiver_spec.mailbox = mailbox_config_for_spawn();
        receiver_spec.dispatch_policy = spawn_receiver->dispatch_policy();
        receiver_spec.dispatch_hints = spawn_receiver->dispatch_hints();
        receiver_spec.reserved_id = SpawnReceiverId;
        receiver_spec.actor_type_override = SystemActorType;
        receiver_spec.origin = SpawnOrigin::System;

        auto spawn_result =
            impl_->spawner->adopt(std::move(spawn_receiver), receiver_spec);
        if (spawn_result.is_error()) {
            impl_->core.running.store(false, std::memory_order_release);
            impl_->network_->stop(NetworkRuntime::StopMode::Abort);
        }
    }

    {
        auto durable_store = std::make_unique<InMemoryStateStore>();
        PassivationConfig defaults;
        impl_->actors.passivation = std::make_unique<PassivationManager>(
            *this, durable_store.release(), defaults);
    }

    // CliActor reads from stdin — only appropriate in foreground mode.
    // In daemon/systemd modes, CLI access goes through CliLegacyServerActor
    // or CliProtoServerActor via UDS/TCP sockets instead.
    if (impl_->core.config.cli.enabled &&
        process::ProcessManager::mode() == process::ProcessMode::Foreground) {
        auto spawned = spawn<cli::CliActor>(impl_->core.config.cli);
        impl_->actors.cli_actor =
            std::static_pointer_cast<cli::CliActor>(spawned.get());
    }

    // Spawn the Receptionist system actor for service-key-based actor
    // discovery. Can be disabled via Config::enable_receptionist.
    if (impl_->core.config.enable_receptionist) {
        auto spawned = spawn<receptionist::Receptionist>();
        impl_->actors.receptionist =
            std::static_pointer_cast<receptionist::Receptionist>(spawned.get());
    }

    impl_->observability_->fault_controller().install();

    impl_->actors.shutdown_coordinator =
        std::make_unique<ShutdownCoordinator>(ShutdownCoordinatorDependencies{
            .phase = &impl_->core.shutdown_phase,
            .running = &impl_->core.running,
            .set_ready =
                [this](bool ready) {
                    impl_->core.is_ready.store(ready, std::memory_order_release);
                },
            .actor_snapshot =
                [this]() {
                    auto entries = impl_->actors.directory.snapshot();
                    std::vector<std::pair<ActorId, bool>> ids;
                    ids.reserve(entries.size());
                    for (const auto& entry : entries) {
                        ids.emplace_back(entry.actor.id(),
                                         entry.instance->is_system_actor());
                    }
                    return ids;
                },
            .get_actor = [this](ActorId id) { return get_actor(id); },
            .get_mailbox_raw = [this](ActorId id) -> void* {
                auto mailbox = impl_->actors.directory.find_mailbox(id);
                return static_cast<void*>(mailbox.get());
            },
            .stop_remote_runtime =
                [this]() {
                    // Lightweight: just stop the event loop so the network
                    // thread exits. Full teardown (join, stop listening,
                    // stop discovery) happens in the destructor.
                    if (auto* loop = event_loop()) {
                        loop->stop();
                    }
                },
            .leave_discovery = []() {},
            .flush_telemetry =
                []() {
                    // Metrics ring buffer is NOT destroyed here — scheduler
                    // workers hold raw pointers to it and may still be running.
                    // The ActorSystem destructor cleans it up after
                    // impl_->core.scheduler->stop() ensures all workers have
                    // exited.
                },
        });
}

ActorSystem::~ActorSystem() {
    impl_->core.running.store(false);
    // Phase 7: Stop cluster before network (cluster depends on discovery).
    if (impl_->cluster_) {
        impl_->cluster_->stop(ClusterStopRequest{});
    }
    // Phase 5: NetworkRuntime handles its own teardown.
    if (impl_->network_) {
        impl_->network_->stop(NetworkRuntime::StopMode::Abort);
    }
    // Legacy fallback: if NetworkRuntime was never created but legacy
    // network fields were populated, clean them up.
    if (impl_->network.event_loop) {
        impl_->network.event_loop->stop();
    }
    if (impl_->network.network_thread.joinable()) {
        impl_->network.network_thread.join();
    }
    if (impl_->network.transport) {
        impl_->network.transport->stop_listening();
    }
    if (impl_->network.discovery) {
        impl_->network.discovery->stop();
    }
    // Stop scheduler BEFORE destroying observability: workers hold raw
    // pointers to the metrics ring buffer and logger owned by
    // ObservabilityRuntime. Workers must quiesce before those pointers
    // become invalid.
    impl_->core.scheduler->stop();
    // Now safe: no worker threads can access metrics/log resources.
    impl_->observability_.reset();
}

void ActorSystem::apply_tracing_config(const tracing::TraceConfig& config) {
    impl_->observability_->apply_tracing_config(config);
}

sched::TimerStatsSnapshot ActorSystem::timer_stats() const {
    auto* hs = static_cast<sched::HybridScheduler*>(impl_->core.scheduler.get());
    return hs->timer_snapshot();
}

void ActorSystem::on_node_dead(EndPoint dead_ep) {
    auto entries = impl_->actors.directory.snapshot();
    for (const auto& entry : entries) {
        if (!entry.context)
            continue;
        for (const auto& addr : entry.context->linked_actors()) {
            if (addr.endpoint == dead_ep) {
                TypedMessage down(TypeTag::DownMsg, StreamBuffer{});
                down.set_sender_address(ActorAddress{dead_ep, 0, ActorId(0), 0});
                deliver_local(entry.actor.id(), std::move(down));
                break;
            }
        }
    }
    if (impl_->network.location_cache)
        impl_->network.location_cache->evict_node(dead_ep);
}

// ── Backpressure API (logging wrappers around BackpressureCoordinator) ───────

void ActorSystem::signal_backpressure(const mailbox::BackpressureSignal& signal) {
    impl_->messaging_->backpressure().deliver_to_sender(signal);
}

void ActorSystem::maybe_emit_backpressure_signal(
    mailbox::MPSCActorMailbox<TypedMessage>* /*mailbox*/,
    const mailbox::EnqueueResult& /*result*/,
    const mailbox::MailboxEnvelopeMeta& /*meta*/, bool /*emit_requested*/,
    mailbox::BackpressureMode /*backpressure_mode*/) {
    static std::once_flag once;
    std::call_once(once, [] {
        HPACTOR_LOG_WARNING(log::LogCategory::kActor, ActorId{0}, 0,
                            "maybe_emit_backpressure_signal is deprecated — "
                            "backpressure is now handled by DeliveryPipeline; "
                            "this call is a no-op");
    });
}

void ActorSystem::emit_local_backpressure_signal(
    const mailbox::BackpressureSignal& signal, mailbox::MailboxPressureState state) {
    if (state == mailbox::MailboxPressureState::HardPressure) {
        HPACTOR_LOG_WARNING(
            log::LogCategory::kMailbox, signal.target.id, 0,
            "backpressure_signal_sent",
            log::field("sender", signal.sender.id.value()),
            log::field("depth", static_cast<uint64_t>(signal.depth)),
            log::field("capacity", static_cast<uint64_t>(signal.capacity)),
            log::field("retry_after_ms",
                       static_cast<uint64_t>(signal.retry_after.count())));
    }
    impl_->messaging_->backpressure().emit_local_signal(signal, state);
}

void ActorSystem::emit_remote_backpressure_signal(
    const mailbox::BackpressureSignal& signal, mailbox::MailboxPressureState state) {
    if (state == mailbox::MailboxPressureState::HardPressure) {
        HPACTOR_LOG_WARNING(
            log::LogCategory::kMailbox, signal.target.id, 0,
            "backpressure_signal_remote_sent",
            log::field("sender", signal.sender.id.value()),
            log::field("depth", static_cast<uint64_t>(signal.depth)),
            log::field("capacity", static_cast<uint64_t>(signal.capacity)),
            log::field("retry_after_ms",
                       static_cast<uint64_t>(signal.retry_after.count())));
    }
    impl_->messaging_->backpressure().emit_remote_signal(signal, state);
    if (!impl_->network.transport &&
        !impl_->network.backpressure_signal_wire_sink_for_test) {
        HPACTOR_LOG_WARNING(log::LogCategory::kMailbox, signal.target.id, 0,
                            "backpressure_signal_remote_send_failed",
                            log::field("sender", signal.sender.id.value()));
    }
}

bool ActorSystem::handle_remote_backpressure_signal(const net::WireFrame& frame) {
    return impl_->messaging_->backpressure().handle_remote_signal(frame);
}

void ActorSystem::set_backpressure_signal_wire_sink_for_test(
    BackpressureSignalWireSink sink) {
    impl_->messaging_->backpressure().set_wire_sink_for_test(std::move(sink));
}

// ── Actor registry ──────────────────────────────────────────────────────────

void ActorSystem::register_actor(const std::string& name, Actor actor) {
    impl_->actors.registry.put(name, actor.address());
}

Actor ActorSystem::resolve_actor(const std::string& name) {
    auto actor_opt = impl_->actors.directory.resolve_actor(name);
    if (actor_opt.has_value()) {
        return actor_opt.value();
    }
    return Actor{};
}

void ActorSystem::unregister_actor(const std::string& name) {
    impl_->actors.registry.erase(name);
}

void ActorSystem::register_actor_type(const ActorTypeDef& def) {
    impl_->actors.types[def.id] = def;
}

ActorTypeDef ActorSystem::get_actor_type(ActorType type) const {
    auto it = impl_->actors.types.find(type);
    if (it != impl_->actors.types.end()) {
        return it->second;
    }
    return ActorTypeDef{};
}

std::shared_ptr<AbstractActor> ActorSystem::get_actor(ActorId id) {
    auto entry = impl_->actors.directory.find(id);
    if (entry.has_value()) {
        return entry->instance;
    }
    return nullptr;
}

mailbox::MPSCActorMailbox<TypedMessage>* ActorSystem::get_mailbox(ActorId id) {
    auto mailbox = impl_->actors.directory.find_mailbox(id);
    return mailbox.get();
}

size_t ActorSystem::actor_count() const {
    return impl_->actors.directory.size();
}

void ActorSystem::for_each_actor(
    std::function<void(ActorId, AbstractActor&)> callback) const {
    auto entries = impl_->actors.directory.snapshot();
    for (const auto& entry : entries) {
        callback(entry.actor.id(), *entry.instance);
    }
}

cli::CliActor* ActorSystem::cli_actor() const {
    return impl_->actors.cli_actor.get();
}

metrics::MetricsActor* ActorSystem::metrics_actor() const {
    return impl_->operations.metrics_actor;
}

receptionist::Receptionist* ActorSystem::receptionist() const {
    return impl_->actors.receptionist.get();
}

// ── Dead-letter queue ───────────────────────────────────────────────────────

bool ActorSystem::dead_letter(mailbox::DeadLetterRecord record) noexcept {
    return impl_->messaging_->dead_letters().try_push(std::move(record));
}

mailbox::DeadLetterQueueSnapshot ActorSystem::dead_letter_snapshot() const noexcept {
    return impl_->messaging_->dead_letters().snapshot();
}

bool ActorSystem::pop_dead_letter(mailbox::DeadLetterRecord& out) noexcept {
    return impl_->messaging_->dead_letters().try_pop(out);
}

mailbox::DeadLetterQueue* ActorSystem::dead_letter_queue() noexcept {
    return &impl_->messaging_->dead_letters();
}

const mailbox::DeadLetterQueue* ActorSystem::dead_letter_queue() const noexcept {
    return &impl_->messaging_->dead_letters();
}

msg::OutboundDeliveryTracker* ActorSystem::outbound_tracker() noexcept {
    return &impl_->messaging_->delivery_receipt_tracker();
}

mailbox::OutboundTracker* ActorSystem::reliable_tracker() noexcept {
    return &impl_->messaging_->mailbox_reliable_tracker();
}

const mailbox::OutboundTracker* ActorSystem::reliable_tracker() const noexcept {
    return &impl_->messaging_->mailbox_reliable_tracker();
}

adt::DedupCache* ActorSystem::dedup_cache() {
    return &impl_->messaging_->dedup_cache();
}

const adt::DedupCache* ActorSystem::dedup_cache() const {
    return &impl_->messaging_->dedup_cache();
}

net::EventLoop* ActorSystem::event_loop() {
    if (impl_->network_)
        return impl_->network_->event_loop();
    return impl_->network.event_loop.get(); // legacy fallback
}

net::Transport* ActorSystem::transport() {
    if (impl_->network_)
        return impl_->network_->transport();
    return impl_->network.transport.get(); // legacy fallback
}

net::UdpRegistrar* ActorSystem::registrar() {
    if (impl_->network_)
        return impl_->network_->registrar();
    return impl_->network.registrar.get(); // legacy fallback
}

RpcChannel& ActorSystem::rpc_channel() {
    if (impl_->network_)
        return *impl_->network_->rpc_channel();
    return *impl_->network.rpc_channel; // legacy fallback
}

tracing::TraceManager* ActorSystem::trace_manager() noexcept {
    return impl_->observability_->trace_manager();
}

const tracing::TraceManager* ActorSystem::trace_manager() const noexcept {
    return impl_->observability_->trace_manager();
}

log::LogManager* ActorSystem::log_manager() noexcept {
    return impl_->observability_->log_manager();
}

const log::LogManager* ActorSystem::log_manager() const noexcept {
    return impl_->observability_->log_manager();
}

fault::FaultController& ActorSystem::fault_controller() noexcept {
    return impl_->observability_->fault_controller();
}

const fault::FaultController& ActorSystem::fault_controller() const noexcept {
    return impl_->observability_->fault_controller();
}

metrics::MpscRingBuffer<metrics::MetricEvent>*
ActorSystem::metrics_ring_buffer() const {
    return impl_->observability_->metrics_ring_buffer();
}

bool ActorSystem::cluster_enabled() const {
    return impl_->cluster.enabled;
}

cluster::ClusterFailureModel* ActorSystem::cluster_failure_model() {
    if (impl_->cluster_) {
        return static_cast<cluster::ClusterFailureModel*>(
            impl_->cluster_->legacy_views().failure_model);
    }
    return nullptr;
}

cluster::singleton::SingletonManagerActor* ActorSystem::singleton_manager() {
    if (impl_->cluster_) {
        return static_cast<cluster::singleton::SingletonManagerActor*>(
            impl_->cluster_->legacy_views().singleton_manager);
    }
    return nullptr;
}

cluster::RouteInvalidation* ActorSystem::route_invalidation() {
    if (impl_->cluster_) {
        return static_cast<cluster::RouteInvalidation*>(
            impl_->cluster_->legacy_views().route_invalidation);
    }
    return nullptr;
}

Clock& ActorSystem::clock() {
    return impl_->core.clock;
}

Actor ActorSystem::system_actor() {
    return impl_->actors.system_actor;
}

ActorSystem::ActorRegistry& ActorSystem::registry() {
    return impl_->actors.registry;
}

ProtoTypeRegistry& ActorSystem::proto_registry() {
    return impl_->core.proto_registry;
}

const ProtoTypeRegistry& ActorSystem::proto_registry() const {
    return impl_->core.proto_registry;
}

EndPoint ActorSystem::endpoint() const {
    return impl_->core.endpoint;
}

bool ActorSystem::is_running() const {
    return impl_->core.running.load(std::memory_order_acquire);
}

std::chrono::milliseconds ActorSystem::uptime() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - impl_->core.start_time);
}

const Config& ActorSystem::config() const {
    return impl_->core.config;
}

sched::IScheduler* ActorSystem::scheduler() {
    return impl_->core.scheduler.get();
}

bool ActorSystem::use_coroutines() const {
    return impl_->core.config.use_coroutines;
}

AskManager* ActorSystem::ask_manager() {
    return impl_->actors.ask.get();
}

const AskManager* ActorSystem::ask_manager() const {
    return impl_->actors.ask.get();
}

PassivationManager* ActorSystem::passivation_manager() {
    return impl_->actors.passivation.get();
}

const PassivationManager* ActorSystem::passivation_manager() const {
    return impl_->actors.passivation.get();
}

net::HttpClient& ActorSystem::http_client() {
    if (impl_->network_)
        return *impl_->network_->http_client();
    return *impl_->actors.http_client; // legacy fallback
}

ActorTypeRegistry& ActorSystem::actor_type_registry() {
    return *impl_->actors.type_registry;
}

const ActorTypeRegistry& ActorSystem::actor_type_registry() const {
    return *impl_->actors.type_registry;
}

ShutdownCoordinator* ActorSystem::shutdown_coordinator() const {
    return impl_->actors.shutdown_coordinator.get();
}

// ── Mailbox config helpers ──────────────────────────────────────────────────

mailbox::MailboxConfig ActorSystem::mailbox_config_for_spawn() const {
    mailbox::MailboxConfig cfg;
    cfg.capacity.max_messages = impl_->core.config.mailbox.default_capacity;
    cfg.capacity.max_bytes = impl_->core.config.mailbox.default_byte_capacity;
    cfg.overflow_policy = impl_->core.config.mailbox.default_policy;
    cfg.high_watermark = impl_->core.config.mailbox.high_watermark;
    cfg.low_watermark = impl_->core.config.mailbox.low_watermark;
    cfg.critical_watermark = impl_->core.config.mailbox.critical_watermark;
    cfg.priority_aware = impl_->core.config.mailbox.priority_aware;
    cfg.priority_levels = impl_->core.config.mailbox.priority_levels;
    cfg.protected_system_messages =
        impl_->core.config.mailbox.protected_system_messages;
    cfg.max_overflow_depth = impl_->core.config.mailbox.max_overflow_depth;
    cfg.signal_min_interval_ms = impl_->core.config.mailbox.signal_min_interval_ms;
    cfg.backpressure_mode = impl_->core.config.mailbox.backpressure_mode;
    return cfg;
}

mailbox::MailboxConfig
ActorSystem::mailbox_config_for_actor_def(const config::ActorDef& def) const {
    auto cfg = mailbox_config_for_spawn();
    if (def.mailbox_capacity != 0) {
        cfg.capacity.max_messages = def.mailbox_capacity;
    }
    if (def.mailbox.policy != mailbox::OverflowPolicy::RejectNewest) {
        cfg.overflow_policy = def.mailbox.policy;
    }
    cfg.priority_aware = def.mailbox.priority_aware;
    cfg.priority_levels = def.mailbox.priority_levels;
    cfg.max_overflow_depth = def.mailbox.max_overflow_depth;
    if (def.mailbox.high_watermark > 0.0) {
        cfg.high_watermark = def.mailbox.high_watermark;
    }
    if (def.mailbox.low_watermark > 0.0) {
        cfg.low_watermark = def.mailbox.low_watermark;
    }
    if (def.mailbox.critical_watermark > 0.0) {
        cfg.critical_watermark = def.mailbox.critical_watermark;
    }
    if (def.mailbox.signal_min_interval_ms != 0) {
        cfg.signal_min_interval_ms = def.mailbox.signal_min_interval_ms;
    }
    cfg.backpressure_mode = def.mailbox.backpressure_mode;
    return cfg;
}

// ── Delivery pipeline (delegates to MessagingRuntime) ────────────────────────

mailbox::EnqueueResult
ActorSystem::try_deliver_local(ActorId target, TypedMessage msg,
                               uint8_t priority, int64_t deadline_ns,
                               mailbox::DeliveryOptions options) {
    return impl_->messaging_->try_deliver(target, std::move(msg), priority,
                                          deadline_ns, options);
}

mailbox::DeliveryResult
ActorSystem::deliver_with_result(ActorId target, TypedMessage msg,
                                 uint8_t priority, int64_t deadline_ns,
                                 mailbox::DeliveryOptions options) {
    return impl_->messaging_->deliver_with_result(target, std::move(msg),
                                                  priority, deadline_ns, options);
}

mailbox::EnqueueResult
ActorSystem::try_deliver_local_fast(ActorId target, TypedMessage msg) {
    return impl_->messaging_->try_deliver_fast(
        target, std::move(msg), FastDeliveryReason::CompatibilityExplicit);
}

void ActorSystem::deliver_local(ActorId target, TypedMessage msg) {
    (void)impl_->messaging_->try_deliver(target, std::move(msg), 0, INT64_MAX, {});
}

void ActorSystem::record_actor_timeout(ActorId target) {
    auto actor_ptr = get_actor(target);
    if (!actor_ptr || !actor_ptr->is_event_based_actor())
        return;
    auto* eba = static_cast<EventBasedActor*>(actor_ptr.get());
    eba->record_circuit_breaker_timeout();
}

void ActorSystem::deliver_local(ActorId target, TypedMessage msg,
                                uint8_t priority, int64_t deadline_ns) {
    (void)impl_->messaging_->try_deliver(target, std::move(msg), priority,
                                         deadline_ns, {});
}

void ActorSystem::deliver_local_edf(ActorId target, TypedMessage msg,
                                    int64_t deadline_ns, uint8_t priority) {
    mailbox::DeliveryOptions options;
    options.schedule_edf = true;
    (void)impl_->messaging_->try_deliver(target, std::move(msg), priority,
                                         deadline_ns, options);
}

void ActorSystem::deliver_remote(const net::WireFrame& frame) {
    // ── Stream frame dispatch ─────────────────────────────────────────────
    switch (frame.payload_type()) {
        case net::WireFrame::PayloadType::StreamOpen:
            deliver_remote_stream_open(frame);
            return;
        case net::WireFrame::PayloadType::StreamData:
            deliver_remote_stream_data(frame);
            return;
        case net::WireFrame::PayloadType::StreamAck:
            deliver_remote_stream_ack(frame);
            return;
        case net::WireFrame::PayloadType::StreamClose:
            deliver_remote_stream_close(frame);
            return;
        case net::WireFrame::PayloadType::StreamError:
            deliver_remote_stream_error(frame);
            return;
        default:
            break; // fall through to existing dispatch
    }

    // ── ACK/NACK control frame dispatch ────────────────────────────────
    constexpr uint32_t kControlAck = 1 << 5;
    constexpr uint32_t kControlNack = 1 << 6;
    uint32_t flags = frame.pb_envelope.data_frame().flags();

    if (flags & kControlAck) {
        impl_->messaging_->on_reliable_ack(
            MessageId{frame.pb_envelope.data_frame().message_id()},
            net::from_proto(frame.pb_envelope.data_frame().sender()).endpoint);
        return;
    }
    if (flags & kControlNack) {
        uint32_t reason_code = frame.pb_envelope.data_frame().type_tag();
        uint32_t retry_after_ms = 0;
        if (frame.pb_envelope.data_frame().payload().size() >= sizeof(uint32_t)) {
            std::memcpy(&retry_after_ms,
                        frame.pb_envelope.data_frame().payload().data(),
                        sizeof(uint32_t));
        }
        impl_->messaging_->on_reliable_nack(
            MessageId{frame.pb_envelope.data_frame().message_id()},
            net::from_proto(frame.pb_envelope.data_frame().sender()).endpoint,
            reason_code, retry_after_ms);
        return;
    }

    if (static_cast<TypeTag>(frame.pb_envelope.data_frame().type_tag()) ==
        TypeTag::BackpressureSignalTag) {
        (void)impl_->messaging_->backpressure().handle_remote_signal(frame);
        return;
    }
    StreamBuffer payload(frame.pb_envelope.data_frame().payload().begin(),
                         frame.pb_envelope.data_frame().payload().end());
    TypedMessage msg(static_cast<TypeTag>(frame.pb_envelope.data_frame().type_tag()),
                     std::move(payload));
    msg.set_sender_address(net::from_proto(frame.pb_envelope.data_frame().sender()));
    if (frame.pb_envelope.data_frame().has_trace_context()) {
        uint16_t max_state = impl_->operations.tracing_config.max_tracestate_len;
        auto parsed = net::trace_context_from_proto(
            frame.pb_envelope.data_frame().trace_context(), max_state);
        if (parsed.has_value()) {
            msg.set_trace_context(parsed.value());
        }
    }
    // ── Preserve AckRequested flag for auto-ACK in downstream pipeline ──
    if (flags & net::WireFrame::AckRequested) {
        msg.set_ack_requested(true);
    }
    msg.set_message_id(frame.pb_envelope.data_frame().message_id());
    deliver_local(net::from_proto(frame.pb_envelope.data_frame().receiver()).id,
                  std::move(msg));
}

void ActorSystem::enqueue_completion(net::OpCompletion completion) {
    StreamBuffer payload(sizeof(net::OpCompletion));
    std::memcpy(payload.data(), &completion, sizeof(net::OpCompletion));
    TypedMessage msg(TypeTag::IoCompletionTag, std::move(payload));
    deliver_local(completion.actor, std::move(msg));
}

net::Transport* ActorSystem::get_transport_for(const EndPoint& /*endpoint*/) {
    if (!impl_->core.config.enable_network) {
        return nullptr;
    }
    if (impl_->network_)
        return impl_->network_->transport();
    return impl_->network.transport.get(); // legacy fallback
}

result<ActorRef> ActorSystem::spawn_remote(const std::string& node_name,
                                           const std::string& actor_type,
                                           const StreamBuffer& args,
                                           RequestTimeout timeout_override) {
    return spawn_remote_async(node_name, actor_type, args, timeout_override).get();
}

RequestHandle<ActorRef>
ActorSystem::spawn_remote_async(const std::string& node_name,
                                const std::string& actor_type,
                                const StreamBuffer& args,
                                RequestTimeout timeout_override) {
    auto state = std::make_shared<RequestHandle<ActorRef>::State>();
    RequestHandle<ActorRef> handle(state);

    if (!impl_->core.config.enable_network || !impl_->network.rpc_channel) {
        handle.resolve_error(error(spawn_errors::node_unreachable, "networking "
                                                                   "disabled"));
        return handle;
    }

    auto remote_endpoint = endpoint_ops::parse_endpoint(node_name);

    ::hpactor::SpawnRequestMessage pb_req;
    pb_req.set_actor_type_name(actor_type);
    pb_req.set_args_type(static_cast<uint32_t>(TypeTag::User));
    pb_req.set_serialized_args(reinterpret_cast<const char*>(args.data()),
                               args.size());
    net::to_proto(pb_req.mutable_supervisor(),
                  impl_->actors.system_actor.address());

    StreamBuffer request_bytes = impl_->core.proto_registry.serialize(pb_req);

    auto timeout_ms = timeout_override.is_default()
                          ? impl_->core.config.spawn_timeout_ms
                          : timeout_override.value;

    ActorAddress target{remote_endpoint, SystemActorType, SpawnReceiverId, 0};

    auto rpc_future =
        impl_->network.rpc_channel->call_raw(target, request_bytes, timeout_ms);

    std::thread([state, fut = std::move(rpc_future)]() mutable {
        RequestHandle<ActorRef> inner(state);
        auto raw_result = fut.get();
        if (!raw_result.has_value()) {
            inner.resolve_error(raw_result.error());
            return;
        }

        ::hpactor::SpawnResponseMessage pb_resp;
        if (!pb_resp.ParseFromArray(raw_result.value().data(),
                                    static_cast<int>(raw_result.value().size()))) {
            inner.resolve_error(error(spawn_errors::deserialization_failed,
                                      "failed to decode SpawnResponse"));
            return;
        }

        if (pb_resp.error_code() != spawn_errors::success) {
            inner.resolve_error(error(pb_resp.error_code(), "spawn failed"));
            return;
        }

        ActorProxy proxy(net::from_proto(pb_resp.actor_addr()),
                         static_cast<net::Transport*>(nullptr));
        ActorRef ref(std::move(proxy));
        inner.resolve(result<ActorRef>::make(std::move(ref)));
    }).detach();

    return handle;
}

// ── adopt_preconstructed_actor
// ────────────────────────────────────────────────

Actor ActorSystem::adopt_preconstructed_actor(std::shared_ptr<AbstractActor> actor,
                                              std::string_view type_name) {
    if (!impl_->spawner.has_value()) {
        return Actor{};
    }

    SpawnSpec spec;
    spec.type_name = type_name;
    spec.mailbox = mailbox_config_for_spawn();
    spec.dispatch_policy = actor->dispatch_policy();
    spec.dispatch_hints = actor->dispatch_hints();
    spec.origin = SpawnOrigin::Programmatic;

    auto result = impl_->spawner->adopt(std::move(actor), spec);
    if (result.is_error()) {
        return Actor{};
    }
    return result.value();
}

// ── spawn_configured ────────────────────────────────────────────────────────

Actor ActorSystem::spawn_configured(std::shared_ptr<AbstractActor> actor,
                                    const config::ActorDef& def) {
    FAULT_INJECT("hpactor.actor.spawn.fail") {
        return {};
    }
    ActorId id = impl_->actors.directory.allocate_id();
    actor->set_address(ActorAddress(impl_->core.endpoint, actor->type(), id, 0));
    actor->set_type_name(def.behavior);

    auto mailbox_ptr = std::make_shared<mailbox::MPSCActorMailbox<TypedMessage>>(
        id, impl_->core.scheduler.get(), mailbox_config_for_actor_def(def));
    auto* mbox = mailbox_ptr.get();

    auto* local = static_cast<LocalActor*>(actor.get());
    auto actor_ctx = std::make_shared<ActorContext>(Actor(actor), this);
    local->set_context(actor_ctx.get());

    ActorDirectoryEntry entry;
    entry.actor = Actor(actor);
    entry.instance = actor;
    entry.mailbox = mailbox_ptr;
    entry.context = actor_ctx;
    impl_->actors.directory.insert(std::move(entry));

    actor->set_scheduler(impl_->core.scheduler.get());
    actor->set_mailbox(mbox);

    if (auto* ring = impl_->observability_->metrics_ring_buffer()) [[unlikely]] {
        mbox->set_metrics_ring_buffer(ring);
        actor->set_metrics_ring_buffer(ring);
    }

    if (auto* logger = impl_->observability_->logger()) [[unlikely]] {
        mbox->set_logger(logger);
        actor->set_logger(logger);
    }

    auto policy = actor->dispatch_policy();
    auto hints = actor->dispatch_hints();
    if (policy == sched::DispatchPolicy::Cooperative) {
        switch (def.dispatch_policy) {
            case config::DispatchPolicy::Cooperative:
                break;
            case config::DispatchPolicy::DedicatedThread:
                policy = sched::DispatchPolicy::DedicatedThread;
                break;
            case config::DispatchPolicy::DedicatedPool:
                policy = sched::DispatchPolicy::DedicatedPool;
                break;
        }
    }

    switch (policy) {
        case sched::DispatchPolicy::Cooperative:
            impl_->core.scheduler->notify_ready(id, 0, INT64_MAX);
            break;
        case sched::DispatchPolicy::DedicatedThread:
            impl_->core.scheduler->register_dedicated_thread(id, hints.cpu_affinity);
            break;
        case sched::DispatchPolicy::DedicatedPool:
            impl_->core.scheduler->register_dedicated_pool(id, hints.pool_size);
            break;
    }

    if (def.quarantine.enabled) {
        if (auto* eba = actor->is_event_based_actor()
                            ? static_cast<EventBasedActor*>(actor.get())
                            : nullptr) {
            eba->configure_quarantine(def.quarantine);
        }
    }

    local->on_activate();

    if (auto* lifecycle = actor->as_lifecycle()) {
        lifecycle->transition(LifecycleState::kActive);
    }

    HPACTOR_LOG_INFO(log::LogCategory::kActor, id,
                     static_cast<uint32_t>(log::LogEventId::kActorSpawned),
                     "actor spawned",
                     log::field_lit("type", actor->type_name().data()));

    if (auto* ring = impl_->observability_->metrics_ring_buffer()) [[unlikely]] {
        metrics::MetricEvent event{};
        event.actor_id = id;
        event.event_type = metrics::MetricEventType::kActorSpawned;
        event.value_hi = 1;
        ring->try_push(event);
    }

    return Actor(actor);
}

// ── load_topology ───────────────────────────────────────────────────────────

result<void> ActorSystem::load_topology(const std::string& toml_path) {
    auto parse_result = config::TomlParser::parse(toml_path);
    if (!parse_result.has_value()) {
        return result<void>::make(parse_result.error());
    }

    auto& model = parse_result.value();

    if (model.system.metrics_enabled) {
        impl_->operations.metrics_config.enabled = model.system.metrics_enabled;
        impl_->operations.metrics_config.ring_buffer_capacity =
            model.system.metrics_ring_buffer_capacity;
        impl_->operations.metrics_config.metrics_path = model.system.metrics_path;
    }

    impl_->operations.logging_config = model.system.logging;

#define HPACTOR_SYSTEM_TOML_FIELD(name, type, toml, def)                       \
    impl_->core.config.name =                                                  \
        static_cast<decltype(impl_->core.config.name)>(model.system.name);
#include <hpactor/config/system_toml_fields.def>
#undef HPACTOR_SYSTEM_TOML_FIELD

#define HPACTOR_MAILBOX_FIELD(name, type, toml, def)                           \
    impl_->core.config.mailbox.name = model.system.mailbox.name;
#include <hpactor/config/mailbox_fields.def>
#undef HPACTOR_MAILBOX_FIELD

    impl_->core.config.dead_letters = model.system.dead_letters;
    impl_->messaging_->reconfigure(impl_->core.config.dead_letters);

    apply_tracing_config(model.system.tracing);

    // Sync process config from TOML (pidfile, watchdog, working directory).
    // Daemonization must have already occurred via the constructor path;
    // this updates in-memory config for notification and signal handling.
    impl_->core.config.process = model.system.process;
#if HPACTOR_ENABLE_AI_ACCELERATORS
    impl_->core.config.ai_accelerators = model.system.ai_accelerators;
#endif

    impl_->core.config.pool.outbound_limits = model.system.transport_outbound_limits;
    impl_->core.config.pool.circuit_breaker_cfg =
        model.system.transport_circuit_breaker;
    if (impl_->network.transport) {
        impl_->network.transport->set_pool_config(impl_->core.config.pool);
    }

    HPACTOR_LOG_INFO(log::LogCategory::kConfig, ActorId{0}, 0,
                     "topology bootstrap complete");

    auto& registry = config::ActorFactoryRegistry::instance();
    for (const auto& actor_def : model.actors) {
        if (!registry.has(actor_def.behavior)) {
            error err(errors::unknown);
            return result<void>::make(std::move(err));
        }
    }

    std::vector<ActorId, mem::MemStdAllocator<ActorId>> spawned_ids(
        mem::MemStdAllocator<ActorId>(impl_->actors.system_actor.id(),
                                      mem::RegionType::kInternal));
    for (const auto& actor_def : model.actors) {
        auto factory = registry.get_factory(actor_def.behavior);
        auto actor_ptr = factory(nullptr, *this);

        Actor actor_handle = spawn_configured(std::move(actor_ptr), actor_def);

        impl_->actors.registry.put(actor_def.id, actor_handle.address());

        spawned_ids.push_back(actor_handle.id());
    }

    ActorAddress sys_addr = impl_->actors.system_actor.address();
    StreamBuffer empty_payload;
    for (ActorId id : spawned_ids) {
        TypedMessage init_msg(TypeTag::SystemInitTag, std::move(empty_payload));
        init_msg.set_sender_address(sys_addr);
        deliver_local(id, std::move(init_msg));
        empty_payload = StreamBuffer{};
    }

    return result<void>::make();
}

// ── Shutdown (delegates to ShutdownCoordinator) ─────────────────────────────

result<void> ActorSystem::shutdown() {
    return shutdown(ShutdownOptions{});
}

result<void> ActorSystem::shutdown(const ShutdownOptions& opts) {
    impl_->actors.shutdown_coordinator->execute(opts);
    return result<void>::make();
}

ShutdownPhase ActorSystem::shutdown_phase() const noexcept {
    return impl_->core.shutdown_phase.load(std::memory_order_acquire);
}

bool ActorSystem::is_ready() const noexcept {
    return impl_->core.is_ready.load(std::memory_order_acquire);
}

bool ActorSystem::is_draining() const noexcept {
    return impl_->core.shutdown_phase.load(std::memory_order_acquire) ==
           ShutdownPhase::DrainingActors;
}

void ActorSystem::set_drain_config(ActorId target, DrainConfig cfg) {
    auto actor = get_actor(target);
    if (actor) {
        if (auto* lc = actor->as_lifecycle()) {
            lc->set_drain_config(cfg);
        }
    }
}

// ── Reliable ACK/NACK frame emission ────────────────────────────────────────

void ActorSystem::send_reliable_ack(const ActorAddress& target,
                                    const ActorAddress& acker, uint64_t msg_id,
                                    uint8_t status, uint32_t retry_after_ms) {
    // Route through the fixed network-control port — same frame construction
    // as reliable_ack_adapter, but called from actor-facing code paths.
    impl_->network.messaging_ports.reliable_ack(target, acker, msg_id, status,
                                                retry_after_ms);
}

// ── Stream protocol ─────────────────────────────────────────────────────────

void ActorSystem::register_stream_sender(uint64_t stream_id, ActorId actor_id) {
    impl_->streams.registry.register_sender(stream_id, actor_id);
}

void ActorSystem::register_stream_receiver(uint64_t stream_id, ActorId actor_id) {
    impl_->streams.registry.register_receiver(stream_id, actor_id);
}

void ActorSystem::unregister_stream(uint64_t stream_id) {
    (void)impl_->streams.registry.take(stream_id);
}

uint64_t ActorSystem::allocate_stream_id(ActorId sender_id) {
    uint64_t seq = impl_->streams.counter.fetch_add(1, std::memory_order_relaxed);
    return (static_cast<uint64_t>(sender_id.value()) << 32) | seq;
}

void ActorSystem::deliver_remote_stream_open(const net::WireFrame& frame) {
    const auto& open = frame.pb_envelope.stream_open();
    ActorId receiver_id = net::from_proto(open.receiver()).id;
    ActorAddress sender_addr = net::from_proto(open.sender());

    TraceContext trace_ctx;
    if (open.has_trace_context()) {
        auto parsed = net::trace_context_from_proto(open.trace_context(), 256);
        if (parsed.has_value())
            trace_ctx = parsed.value();
    }

    auto receiver =
        spawn<StreamReceiverActor>(receiver_id, open.stream_id(), sender_addr,
                                   open.initial_window_bytes(), trace_ctx);

    if (receiver) {
        register_stream_receiver(open.stream_id(), receiver.id());

        // Deliver StreamOpenedTag so the target actor knows a stream session
        // has been established (mirrors the local path in open_stream_impl).
        TypedMessage open_msg(stream::StreamOpenedTag, StreamBuffer{});
        (void)impl_->messaging_->try_deliver_fast(
            receiver_id, std::move(open_msg), FastDeliveryReason::StreamProtocol);
    }
}

void ActorSystem::deliver_remote_stream_data(const net::WireFrame& frame) {
    const auto& data = frame.pb_envelope.stream_data();
    auto receiver = impl_->streams.registry.find_receiver(data.stream_id());
    if (!receiver.has_value())
        return;

    const auto& payload_str = data.payload();
    auto payload = StreamBuffer::from_data(
        reinterpret_cast<const uint8_t*>(payload_str.data()), payload_str.size());
    TypedMessage msg(stream::StreamDataTag, std::move(payload));
    (void)impl_->messaging_->try_deliver_fast(receiver.value(), std::move(msg),
                                              FastDeliveryReason::StreamProtocol);
}

void ActorSystem::deliver_remote_stream_ack(const net::WireFrame& frame) {
    const auto& ack = frame.pb_envelope.stream_ack();
    auto sender = impl_->streams.registry.find_sender(ack.stream_id());
    if (!sender.has_value())
        return;

    // Serialize to wire format so msg.as<StreamAckFrame>() can parse correctly.
    TypedMessage msg(stream::StreamAckTag, ack);
    (void)impl_->messaging_->try_deliver_fast(sender.value(), std::move(msg),
                                              FastDeliveryReason::StreamProtocol);
}

void ActorSystem::deliver_remote_stream_close(const net::WireFrame& frame) {
    const auto& close = frame.pb_envelope.stream_close();
    auto routes = impl_->streams.registry.take(close.stream_id());
    if (routes.sender.has_value()) {
        TypedMessage msg(stream::StreamClosedTag, StreamBuffer{});
        (void)impl_->messaging_->try_deliver_fast(
            routes.sender.value(), std::move(msg),
            FastDeliveryReason::StreamProtocol);
    }
    if (routes.receiver.has_value()) {
        TypedMessage msg(stream::StreamClosedTag, StreamBuffer{});
        (void)impl_->messaging_->try_deliver_fast(
            routes.receiver.value(), std::move(msg),
            FastDeliveryReason::StreamProtocol);
    }
}

void ActorSystem::deliver_remote_stream_error(const net::WireFrame& frame) {
    const auto& error = frame.pb_envelope.stream_error();
    auto routes = impl_->streams.registry.take(error.stream_id());
    if (routes.sender.has_value()) {
        TypedMessage msg(stream::StreamErrorTag, StreamBuffer{});
        (void)impl_->messaging_->try_deliver_fast(
            routes.sender.value(), std::move(msg),
            FastDeliveryReason::StreamProtocol);
    }
    if (routes.receiver.has_value()) {
        TypedMessage msg(stream::StreamErrorTag, StreamBuffer{});
        (void)impl_->messaging_->try_deliver_fast(
            routes.receiver.value(), std::move(msg),
            FastDeliveryReason::StreamProtocol);
    }
}

// Static delivery callback for StreamHandle → StreamSenderActor communication.
// Bound to an ActorSystem* context in open_stream().
namespace {
bool deliver_to_stream_sender(void* ctx, ActorId target, TypedMessage msg) {
    auto* sys = static_cast<ActorSystem*>(ctx);
    // try_deliver_local_fast returns EnqueueResult; accepted() means success.
    return sys->try_deliver_local_fast(target, std::move(msg)).accepted();
}
} // namespace

std::optional<StreamHandle>
ActorSystem::open_stream(ActorId target, StreamConfig config) {
    auto actor = get_actor(target);
    if (!actor)
        return std::nullopt;

    ActorAddress target_addr = actor->address();
    bool is_local_target = target_addr.endpoint == impl_->core.endpoint;
    TraceContext trace_ctx;
    auto shared_state = std::make_shared<StreamSenderState>();

    return open_stream_impl(target, target_addr, is_local_target, config,
                            trace_ctx, shared_state);
}

std::optional<StreamHandle>
ActorSystem::open_stream(ActorRef target, StreamConfig config) {
    if (target.is_local()) {
        auto* actor = target.get_actor();
        if (!actor || !actor->get())
            return std::nullopt;
        return open_stream(actor->get()->id(), config);
    }

    // Remote target via ActorProxy.
    auto* proxy = target.get_proxy();
    if (!proxy)
        return std::nullopt;

    ActorAddress target_addr = proxy->address();
    TraceContext trace_ctx;
    auto shared_state = std::make_shared<StreamSenderState>();

    return open_stream_impl(target_addr.id, target_addr,
                            /*is_local_target=*/false, config, trace_ctx,
                            shared_state);
}

std::optional<StreamHandle>
ActorSystem::open_stream_impl(ActorId receiver_id, ActorAddress receiver_addr,
                              bool is_local_target, StreamConfig config,
                              TraceContext trace_ctx,
                              std::shared_ptr<StreamSenderState> shared_state) {
    uint64_t stream_id = allocate_stream_id(receiver_id);

    // Spawn StreamSenderActor with local/remote awareness.
    auto sender =
        spawn<StreamSenderActor>(receiver_id, receiver_addr, stream_id, config,
                                 trace_ctx, is_local_target, shared_state);
    if (!sender)
        return std::nullopt;
    register_stream_sender(stream_id, sender.id());

    // Create StreamHandle with the delivery callback bound to this ActorSystem.
    StreamHandle handle(sender.id(), stream_id, deliver_to_stream_sender, this,
                        shared_state);

    if (is_local_target) {
        // Local target: spawn StreamReceiverActor directly.
        ActorAddress sender_addr{impl_->core.endpoint, ActorType{0}, ActorId{0}, 0};
        auto receiver =
            spawn<StreamReceiverActor>(receiver_id, stream_id, sender_addr,
                                       config.initial_window_bytes, trace_ctx);
        if (receiver) {
            register_stream_receiver(stream_id, receiver.id());
        }

        // Deliver StreamOpenedTag to the target actor so it knows
        // a stream session has been established.
        TypedMessage open_msg(stream::StreamOpenedTag, StreamBuffer{});
        (void)impl_->messaging_->try_deliver_fast(
            receiver_id, std::move(open_msg), FastDeliveryReason::StreamProtocol);
    } else {
        // Remote target: send StreamOpenFrame via transport.
        auto* tp = transport();
        if (tp) {
            net::StreamOpenFrame open;
            open.set_stream_id(stream_id);
            net::to_proto(open.mutable_sender(),
                          ActorAddress{impl_->core.endpoint, ActorType{0},
                                       ActorId{0}, 0});
            net::to_proto(open.mutable_receiver(), receiver_addr);
            open.set_initial_window_bytes(config.initial_window_bytes);
            if (trace_ctx.valid()) {
                net::to_proto(open.mutable_trace_context(), trace_ctx);
            }
            auto wire_frame = net::WireFrame::from_stream_open(std::move(open));
            (void)tp->try_send(receiver_addr, wire_frame.encode());
        }
    }

    return handle;
}

} // namespace hpactor
