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

// -----------------------------------------------------------------------------
// ActorSystem implementation
// -----------------------------------------------------------------------------
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
    impl_->spawner.emplace(ActorSpawner::Dependencies{
        .facade = *this,
        .endpoint = impl_->core.endpoint,
        .directory = impl_->actors.directory,
        .scheduler = *impl_->core.scheduler,
        .metrics = impl_->operations.metrics_ring_buffer.get(),
        .logger = impl_->operations.logger,
    });
    // ── System protobuf types ──────────────────────────────────────────
    impl_->core.proto_registry.register_system_types();

    // ── Dead-letter queue ──────────────────────────────────────────────
    impl_->messaging.dead_letters =
        std::make_unique<mailbox::DeadLetterQueue>(impl_->core.config.dead_letters);

    // ── Receiver dedup cache ───────────────────────────────────────────
    impl_->messaging.dedup_cache =
        std::make_unique<adt::DedupCache>(adt::DedupCache::Config{});

    // ── Extracted runtime components ───────────────────────────────────
    impl_->messaging.local_delivery_engine =
        std::make_unique<LocalDeliveryEngine>(impl_->actors.directory);
    {
        BackpressureCoordinator::Config bp_cfg;
        bp_cfg.metrics_ring_buffer = nullptr;
        bp_cfg.transport = nullptr;
        bp_cfg.actor_directory = &impl_->actors.directory;
        bp_cfg.endpoint = impl_->core.endpoint;
        impl_->messaging.backpressure =
            std::make_unique<BackpressureCoordinator>(std::move(bp_cfg));
    }

    // ── Outbound delivery tracker ─────────────────────────────────────
    impl_->messaging.outbound_tracker =
        std::make_unique<msg::OutboundDeliveryTracker>();

    // ── Reliable messaging outbound tracker ───────────────────────────
    impl_->messaging.reliable_tracker =
        std::make_unique<mailbox::OutboundTracker>(mailbox::ReliableRetryPolicy{});

    // ── Delivery pipeline ──────────────────────────────────────────────
    // MUST be created before the scheduler starts so that early-boot
    // actor spawns can deliver messages through it.
    {
        mailbox::DeliveryPipeline::Config pipeline_cfg;
        pipeline_cfg.dlq = impl_->messaging.dead_letters.get();
        pipeline_cfg.outbound_tracker = impl_->messaging.outbound_tracker.get();
        pipeline_cfg.metrics = nullptr;
        pipeline_cfg.dedup_cache = impl_->messaging.dedup_cache.get();
        pipeline_cfg.endpoint = impl_->core.endpoint;
        pipeline_cfg.default_message_ttl_ms =
            impl_->core.config.default_message_ttl_ms;

        pipeline_cfg.get_actor = [this](ActorId id) {
            auto entry = impl_->actors.directory.find(id);
            return entry.has_value() ? entry->instance : nullptr;
        };
        pipeline_cfg.get_mailbox = [this](ActorId id) {
            auto mailbox = impl_->actors.directory.find_mailbox(id);
            return mailbox.get();
        };
        pipeline_cfg.emit_local_backpressure =
            [this](const mailbox::BackpressureSignal& signal,
                   mailbox::MailboxPressureState state) {
                emit_local_backpressure_signal(signal, state);
            };
        pipeline_cfg.emit_remote_backpressure =
            [this](const mailbox::BackpressureSignal& signal,
                   mailbox::MailboxPressureState state) {
                emit_remote_backpressure_signal(signal, state);
            };

        // ── Auto-ACK callback: delegate to send_reliable_ack ──
        pipeline_cfg.emit_ack = [this](const ActorAddress& sender, uint64_t msg_id,
                                       uint8_t status, uint32_t retry_after_ms) {
            // The acker is the local endpoint (the pipeline doesn't know
            // which specific actor).  For ACK/NACK, the endpoint + msg_id
            // is sufficient identification.
            ActorAddress acker{impl_->core.endpoint, ActorType{0}, ActorId{0}, 0};
            send_reliable_ack(sender, acker, msg_id, status, retry_after_ms);
        };

        impl_->messaging.delivery_pipeline =
            std::make_unique<mailbox::DeliveryPipeline>(std::move(pipeline_cfg));
    }

    // ── Metrics subsystem ──────────────────────────────────────────────
    if (impl_->operations.metrics_config.enabled) {
        impl_->operations.metrics_ring_buffer =
            std::make_shared<metrics::MpscRingBuffer<metrics::MetricEvent>>();
        impl_->core.scheduler->set_metrics_ring_buffer(
            impl_->operations.metrics_ring_buffer.get());
        impl_->messaging.delivery_pipeline->set_metrics(
            impl_->operations.metrics_ring_buffer.get());
        impl_->messaging.backpressure->set_metrics_ring_buffer(
            impl_->operations.metrics_ring_buffer.get());

        auto m_actor =
            spawn<metrics::MetricsActor>(impl_->operations.metrics_ring_buffer);
        impl_->operations.metrics_actor =
            static_cast<metrics::MetricsActor*>(m_actor.get().get());
    }

    // ── Logging subsystem ──────────────────────────────────────────────
    if (impl_->operations.logging_config.enabled) {
        impl_->operations.log_manager =
            std::make_unique<log::LogManager>(impl_->operations.logging_config);
        impl_->operations.log_manager->start();
        impl_->operations.logger = &impl_->operations.log_manager->logger();
    }

    if (impl_->operations.logger) [[unlikely]] {
        impl_->core.scheduler->set_logger(impl_->operations.logger);
    }

    impl_->operations.fault_controller.set_log_manager(
        impl_->operations.log_manager.get());

    // Initialize process-mode subystem (daemonize, pidfile, signal handling).
    // Must happen before impl_->core.scheduler->start() so daemonization forks
    // before any worker threads are created.
    (void)process::ProcessManager::init(impl_->core.config.process);

    impl_->core.scheduler->start();

    apply_tracing_config(impl_->core.config.tracing);

    if (config.enable_network) {
        impl_->network.event_loop = std::make_unique<net::EventLoop>();
        impl_->network.event_loop->set_actor_system(this);

        if (config.service_discovery) {
            impl_->network.discovery = config.service_discovery;
        } else if (config.registrar.udp_port > 0) {
            auto reg = std::make_shared<net::UdpRegistrar>(
                config.registrar, impl_->core.endpoint,
                impl_->network.event_loop.get());
            impl_->network.discovery = reg;
            impl_->network.registrar = reg;
        } else {
            impl_->network.discovery =
                std::make_shared<net::StaticDiscovery>(std::vector<net::Member>{});
        }

        impl_->network.discovery->start();

        impl_->network.discovery->on_member_change(
            [this](const net::Member& m, bool joined) {
                if (!joined) {
                    on_node_dead(m.identity.endpoint);
                }
            });

        impl_->network.location_cache =
            std::make_shared<net::ActorLocationCache>();
        if (impl_->network.event_loop) {
            impl_->network.cache_purge_timer = impl_->network.event_loop->run_every(
                [this]() {
                    if (impl_->network.location_cache)
                        impl_->network.location_cache->purge_expired();
                },
                60000);
        }

        // Periodic retry processing for at-least-once delivery.
        if (impl_->messaging.outbound_tracker && impl_->network.event_loop) {
            impl_->network.retry_timer = impl_->network.event_loop->run_every(
                [this]() {
                    if (impl_->messaging.outbound_tracker) {
                        uint64_t now_ns = static_cast<uint64_t>(
                            std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count());
                        impl_->messaging.outbound_tracker->process_retries(
                            now_ns,
                            [](const msg::OutboundDeliveryTracker::PendingSend&) {
                                // Resend via transport (implemented in
                                // follow-up).
                            });
                    }
                },
                100); // poll every 100ms
        }

        impl_->network.transport = std::make_unique<net::TcpTransport>(
            impl_->core.endpoint, config.tls, config.pool, nullptr);
        impl_->messaging.backpressure->set_transport(impl_->network.transport.get());

        if (impl_->operations.metrics_ring_buffer) {
            impl_->network.transport->set_metrics_ring_buffer(
                impl_->operations.metrics_ring_buffer.get());
        }

        impl_->network.rpc_channel = std::make_unique<RpcChannel>(
            impl_->network.transport.get(), impl_->core.scheduler.get(),
            impl_->core.config.default_ask_max_retries);

        impl_->actors.ask =
            std::make_unique<AskManager>(impl_->core.scheduler.get(), this);

        if (impl_->core.config.enable_http_client) {
            impl_->actors.http_client =
                std::make_unique<net::HttpClient>(impl_->network.event_loop.get());
        }

        if (impl_->core.config.enable_http_gateway) {
            impl_->actors.http_gateway_actor = spawn<net::HTTPGatewayActor>(
                impl_->core.config.http_bind_host, impl_->core.config.http_port);
        }

        if (config.tcp_port > 0) {
            impl_->network.transport->set_rpc_handler(
                [this](const hpactor::RpcResponseFrame& response) {
                    impl_->network.rpc_channel->on_response(response);
                });
            impl_->network.transport->set_actor_message_handler(
                [this](const net::WireFrame& frame) {
                    this->deliver_remote(frame);
                });
            impl_->network.transport->listen(config.tcp_port);
        }

        impl_->network.network_thread = std::thread([this]() {
            while (impl_->network.event_loop->wait(100) >= 0) {
                impl_->network.event_loop->process_completions();
                if (!is_running())
                    break;
            }
        });

        auto spawn_receiver = std::make_shared<SpawnReceiver>(
            *this, *impl_->actors.type_registry, impl_->network.transport.get());
        spawn_receiver->set_address(ActorAddress{
            impl_->core.endpoint, SystemActorType, SpawnReceiverId, 0});

        auto spawn_mailbox =
            std::make_shared<mailbox::MPSCActorMailbox<TypedMessage>>(
                SpawnReceiverId, impl_->core.scheduler.get(),
                mailbox_config_for_spawn());
        auto spawn_ctx =
            std::make_shared<ActorContext>(Actor(spawn_receiver), this);
        spawn_receiver->set_context(spawn_ctx.get());

        ActorDirectoryEntry spawn_entry;
        spawn_entry.actor = Actor(spawn_receiver);
        spawn_entry.instance = spawn_receiver;
        spawn_entry.mailbox = spawn_mailbox;
        spawn_entry.context = spawn_ctx;
        impl_->actors.directory.insert(std::move(spawn_entry));
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

    impl_->operations.fault_controller.install();

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
                    if (impl_->network.event_loop) {
                        impl_->network.event_loop->stop();
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
    if (impl_->core.config.enable_network) {
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
    }
    if (impl_->operations.log_manager) {
        impl_->operations.log_manager->stop();
    }
    if (impl_->operations.trace_manager) {
        impl_->operations.trace_manager->stop();
    }
    impl_->core.scheduler->stop();
    impl_->operations.fault_controller.remove();
}

void ActorSystem::apply_tracing_config(const tracing::TraceConfig& config) {
    impl_->operations.tracing_config = config;
    if (!impl_->operations.tracing_config.enabled) {
        if (impl_->operations.trace_manager) {
            impl_->operations.trace_manager->stop();
            impl_->operations.trace_manager.reset();
        }
        return;
    }
    impl_->operations.trace_manager = std::make_unique<tracing::TraceManager>(
        impl_->operations.tracing_config, this);
    impl_->operations.trace_manager->start();
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
    impl_->messaging.backpressure->deliver_to_sender(signal);
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
    impl_->messaging.backpressure->emit_local_signal(signal, state);
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
    impl_->messaging.backpressure->emit_remote_signal(signal, state);
    if (!impl_->network.transport &&
        !impl_->network.backpressure_signal_wire_sink_for_test) {
        HPACTOR_LOG_WARNING(log::LogCategory::kMailbox, signal.target.id, 0,
                            "backpressure_signal_remote_send_failed",
                            log::field("sender", signal.sender.id.value()));
    }
}

bool ActorSystem::handle_remote_backpressure_signal(const net::WireFrame& frame) {
    return impl_->messaging.backpressure->handle_remote_signal(frame);
}

void ActorSystem::set_backpressure_signal_wire_sink_for_test(
    BackpressureSignalWireSink sink) {
    impl_->messaging.backpressure->set_wire_sink_for_test(std::move(sink));
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
    if (!impl_->messaging.dead_letters) {
        return false;
    }
    return impl_->messaging.dead_letters->try_push(std::move(record));
}

mailbox::DeadLetterQueueSnapshot ActorSystem::dead_letter_snapshot() const noexcept {
    if (!impl_->messaging.dead_letters) {
        return {};
    }
    return impl_->messaging.dead_letters->snapshot();
}

bool ActorSystem::pop_dead_letter(mailbox::DeadLetterRecord& out) noexcept {
    if (!impl_->messaging.dead_letters) {
        return false;
    }
    return impl_->messaging.dead_letters->try_pop(out);
}

mailbox::DeadLetterQueue* ActorSystem::dead_letter_queue() noexcept {
    return impl_->messaging.dead_letters.get();
}

const mailbox::DeadLetterQueue* ActorSystem::dead_letter_queue() const noexcept {
    return impl_->messaging.dead_letters.get();
}

msg::OutboundDeliveryTracker* ActorSystem::outbound_tracker() noexcept {
    return impl_->messaging.outbound_tracker.get();
}

mailbox::OutboundTracker* ActorSystem::reliable_tracker() noexcept {
    return impl_->messaging.reliable_tracker.get();
}

const mailbox::OutboundTracker* ActorSystem::reliable_tracker() const noexcept {
    return impl_->messaging.reliable_tracker.get();
}

adt::DedupCache* ActorSystem::dedup_cache() {
    return impl_->messaging.dedup_cache.get();
}

const adt::DedupCache* ActorSystem::dedup_cache() const {
    return impl_->messaging.dedup_cache.get();
}

net::EventLoop* ActorSystem::event_loop() {
    return impl_->network.event_loop.get();
}

net::Transport* ActorSystem::transport() {
    return impl_->network.transport.get();
}

net::UdpRegistrar* ActorSystem::registrar() {
    return impl_->network.registrar.get();
}

RpcChannel& ActorSystem::rpc_channel() {
    return *impl_->network.rpc_channel;
}

tracing::TraceManager* ActorSystem::trace_manager() noexcept {
    return impl_->operations.trace_manager.get();
}

const tracing::TraceManager* ActorSystem::trace_manager() const noexcept {
    return impl_->operations.trace_manager.get();
}

log::LogManager* ActorSystem::log_manager() noexcept {
    return impl_->operations.log_manager.get();
}

const log::LogManager* ActorSystem::log_manager() const noexcept {
    return impl_->operations.log_manager.get();
}

fault::FaultController& ActorSystem::fault_controller() noexcept {
    return impl_->operations.fault_controller;
}

const fault::FaultController& ActorSystem::fault_controller() const noexcept {
    return impl_->operations.fault_controller;
}

metrics::MpscRingBuffer<metrics::MetricEvent>*
ActorSystem::metrics_ring_buffer() const {
    return impl_->operations.metrics_ring_buffer.get();
}

bool ActorSystem::cluster_enabled() const {
    return impl_->cluster.enabled;
}

cluster::ClusterFailureModel* ActorSystem::cluster_failure_model() {
    return static_cast<cluster::ClusterFailureModel*>(
        impl_->cluster.failure_model.get());
}

cluster::singleton::SingletonManagerActor* ActorSystem::singleton_manager() {
    return static_cast<cluster::singleton::SingletonManagerActor*>(
        impl_->cluster.singleton_manager.get());
}

cluster::RouteInvalidation* ActorSystem::route_invalidation() {
    return static_cast<cluster::RouteInvalidation*>(
        impl_->cluster.route_invalidation.get());
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
    return *impl_->actors.http_client;
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

// ── Delivery pipeline (delegates to DeliveryPipeline) ───────────────────────

mailbox::EnqueueResult
ActorSystem::try_deliver_local(ActorId target, TypedMessage msg,
                               uint8_t priority, int64_t deadline_ns,
                               mailbox::DeliveryOptions options) {
    return impl_->messaging.delivery_pipeline->try_deliver(
        target, std::move(msg), priority, deadline_ns, options);
}

mailbox::DeliveryResult
ActorSystem::deliver_with_result(ActorId target, TypedMessage msg,
                                 uint8_t priority, int64_t deadline_ns,
                                 mailbox::DeliveryOptions options) {
    return impl_->messaging.delivery_pipeline->deliver_with_result(
        target, std::move(msg), priority, deadline_ns, options);
}

mailbox::EnqueueResult
ActorSystem::try_deliver_local_fast(ActorId target, TypedMessage msg) {
    auto* mailbox = get_mailbox(target);
    if (!mailbox) {
        mailbox::EnqueueResult r;
        r.code = mailbox::EnqueueResultCode::ActorNotFound;
        r.target = target;
        return r;
    }
    mailbox::MailboxEnvelopeMeta meta;
    meta.sender = msg.sender_address();
    meta.type_tag = msg.type_id();
    meta.priority = 0;
    meta.deadline_ns = INT64_MAX;
    return mailbox->try_push(std::move(msg), meta);
}

void ActorSystem::deliver_local(ActorId target, TypedMessage msg) {
    if (!impl_->messaging.delivery_pipeline)
        return;
    (void)impl_->messaging.delivery_pipeline->try_deliver(target, std::move(msg));
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
    (void)impl_->messaging.delivery_pipeline->try_deliver(
        target, std::move(msg), priority, deadline_ns, {});
}

void ActorSystem::deliver_local_edf(ActorId target, TypedMessage msg,
                                    int64_t deadline_ns, uint8_t priority) {
    if (!impl_->messaging.delivery_pipeline)
        return;
    mailbox::DeliveryOptions options;
    options.schedule_edf = true;
    (void)impl_->messaging.delivery_pipeline->try_deliver(
        target, std::move(msg), priority, deadline_ns, options);
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

    if ((flags & kControlAck) && impl_->messaging.outbound_tracker) {
        impl_->messaging.outbound_tracker->on_ack(
            MessageId{frame.pb_envelope.data_frame().message_id()},
            net::from_proto(frame.pb_envelope.data_frame().sender()).endpoint);
        return;
    }
    if ((flags & kControlNack) && impl_->messaging.outbound_tracker) {
        uint32_t reason_code = frame.pb_envelope.data_frame().type_tag();
        uint32_t retry_after_ms = 0;
        if (frame.pb_envelope.data_frame().payload().size() >= sizeof(uint32_t)) {
            std::memcpy(&retry_after_ms,
                        frame.pb_envelope.data_frame().payload().data(),
                        sizeof(uint32_t));
        }
        impl_->messaging.outbound_tracker->on_nack(
            MessageId{frame.pb_envelope.data_frame().message_id()},
            net::from_proto(frame.pb_envelope.data_frame().sender()).endpoint,
            reason_code, retry_after_ms);
        return;
    }

    if (static_cast<TypeTag>(frame.pb_envelope.data_frame().type_tag()) ==
        TypeTag::BackpressureSignalTag) {
        (void)impl_->messaging.backpressure->handle_remote_signal(frame);
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
    return impl_->network.transport.get();
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

    if (impl_->operations.metrics_ring_buffer) [[unlikely]] {
        mbox->set_metrics_ring_buffer(impl_->operations.metrics_ring_buffer.get());
        actor->set_metrics_ring_buffer(impl_->operations.metrics_ring_buffer.get());
    }

    if (impl_->operations.logger) [[unlikely]] {
        mbox->set_logger(impl_->operations.logger);
        actor->set_logger(impl_->operations.logger);
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

    if (impl_->operations.metrics_ring_buffer) [[unlikely]] {
        metrics::MetricEvent event{};
        event.actor_id = id;
        event.event_type = metrics::MetricEventType::kActorSpawned;
        event.value_hi = 1;
        impl_->operations.metrics_ring_buffer->try_push(event);
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
    if (impl_->messaging.dead_letters) {
        impl_->messaging.dead_letters->reconfigure(impl_->core.config.dead_letters);
    }

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
    if (!impl_->network.transport) {
        return;
    }

    net::WireFrame frame;
    // For ACK (Accepted/Duplicate): use AckRequested flag (kControlAck on
    // receiver side).  For NACK (Rejected): use AckResponse flag (kControlNack
    // on receiver side).  This matches the convention in deliver_remote().
    bool is_nack = (status == 1); // 1 = AckStatus::Rejected
    frame.pb_envelope.mutable_data_frame()->set_flags(
        is_nack ? net::WireFrame::AckResponse : net::WireFrame::AckRequested);
    frame.pb_envelope.mutable_data_frame()->set_message_id(msg_id);
    net::to_proto(frame.pb_envelope.mutable_data_frame()->mutable_sender(), acker);
    net::to_proto(frame.pb_envelope.mutable_data_frame()->mutable_receiver(),
                  target);

    if (is_nack) {
        // Encode reason code in type_tag (read as reason_code by receiver)
        frame.pb_envelope.mutable_data_frame()->set_type_tag(
            static_cast<uint32_t>(status));
        // Encode retry_after_ms as 4-byte little-endian in payload
        std::string payload_str(reinterpret_cast<const char*>(&retry_after_ms),
                                sizeof(uint32_t));
        frame.pb_envelope.mutable_data_frame()->set_payload(payload_str);
    }

    auto encoded = frame.encode();
    (void)impl_->network.transport->try_send(target, encoded);
}

// ── Stream protocol ─────────────────────────────────────────────────────────

void ActorSystem::register_stream_sender(uint64_t stream_id, ActorId actor_id) {
    impl_->messaging.stream_registry.register_sender(stream_id, actor_id);
}

void ActorSystem::register_stream_receiver(uint64_t stream_id, ActorId actor_id) {
    impl_->messaging.stream_registry.register_receiver(stream_id, actor_id);
}

void ActorSystem::unregister_stream(uint64_t stream_id) {
    (void)impl_->messaging.stream_registry.take(stream_id);
}

uint64_t ActorSystem::allocate_stream_id(ActorId sender_id) {
    uint64_t seq =
        impl_->messaging.stream_counter.fetch_add(1, std::memory_order_relaxed);
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
    }
}

void ActorSystem::deliver_remote_stream_data(const net::WireFrame& frame) {
    const auto& data = frame.pb_envelope.stream_data();
    auto receiver =
        impl_->messaging.stream_registry.find_receiver(data.stream_id());
    if (!receiver.has_value())
        return;

    const auto& payload_str = data.payload();
    auto payload = StreamBuffer::from_data(
        reinterpret_cast<const uint8_t*>(payload_str.data()), payload_str.size());
    TypedMessage msg(stream::StreamDataTag, std::move(payload));
    (void)try_deliver_local_fast(receiver.value(), std::move(msg));
}

void ActorSystem::deliver_remote_stream_ack(const net::WireFrame& frame) {
    const auto& ack = frame.pb_envelope.stream_ack();
    auto sender = impl_->messaging.stream_registry.find_sender(ack.stream_id());
    if (!sender.has_value())
        return;

    auto ack_buf = StreamBuffer::from_data(reinterpret_cast<const uint8_t*>(&ack),
                                           sizeof(ack));
    TypedMessage msg(stream::StreamAckTag, std::move(ack_buf));
    (void)try_deliver_local_fast(sender.value(), std::move(msg));
}

void ActorSystem::deliver_remote_stream_close(const net::WireFrame& frame) {
    const auto& close = frame.pb_envelope.stream_close();
    auto routes = impl_->messaging.stream_registry.take(close.stream_id());
    if (routes.sender.has_value()) {
        TypedMessage msg(stream::StreamClosedTag, StreamBuffer{});
        (void)try_deliver_local_fast(routes.sender.value(), std::move(msg));
    }
    if (routes.receiver.has_value()) {
        TypedMessage msg(stream::StreamClosedTag, StreamBuffer{});
        (void)try_deliver_local_fast(routes.receiver.value(), std::move(msg));
    }
}

void ActorSystem::deliver_remote_stream_error(const net::WireFrame& frame) {
    const auto& error = frame.pb_envelope.stream_error();
    auto routes = impl_->messaging.stream_registry.take(error.stream_id());
    if (routes.sender.has_value()) {
        TypedMessage msg(stream::StreamErrorTag, StreamBuffer{});
        (void)try_deliver_local_fast(routes.sender.value(), std::move(msg));
    }
    if (routes.receiver.has_value()) {
        TypedMessage msg(stream::StreamErrorTag, StreamBuffer{});
        (void)try_deliver_local_fast(routes.receiver.value(), std::move(msg));
    }
}

std::optional<StreamHandle>
ActorSystem::open_stream(ActorId target, StreamConfig config) {
    auto actor = get_actor(target);
    if (!actor)
        return std::nullopt;

    uint64_t stream_id = allocate_stream_id(target);
    TraceContext trace_ctx; // Default: no trace context

    // Resolve target address
    ActorAddress target_addr = actor->address();
    bool is_local_target = target_addr.endpoint == impl_->core.endpoint;

    // Spawn StreamSenderActor
    auto sender = spawn<StreamSenderActor>(target, stream_id, config, trace_ctx);
    if (!sender)
        return std::nullopt;
    register_stream_sender(stream_id, sender.id());

    if (is_local_target) {
        // Local target: spawn StreamReceiverActor directly
        auto receiver = spawn<StreamReceiverActor>(
            target, stream_id,
            ActorAddress{impl_->core.endpoint, ActorType{0}, ActorId{0}, 0},
            config.initial_window_bytes, trace_ctx);
        if (receiver) {
            register_stream_receiver(stream_id, receiver.id());
        }
    } else {
        // Remote target: send StreamOpenFrame via transport
        auto* tp = transport();
        if (tp) {
            net::StreamOpenFrame open;
            open.set_stream_id(stream_id);
            net::to_proto(open.mutable_sender(),
                          ActorAddress{impl_->core.endpoint, ActorType{0},
                                       ActorId{0}, 0});
            net::to_proto(open.mutable_receiver(), target_addr);
            open.set_initial_window_bytes(config.initial_window_bytes);
            if (trace_ctx.valid()) {
                net::to_proto(open.mutable_trace_context(), trace_ctx);
            }
            auto wire_frame = net::WireFrame::from_stream_open(std::move(open));
            (void)tp->try_send(target_addr, wire_frame.encode());
        }
    }

    return StreamHandle(sender.id(), stream_id);
}

} // namespace hpactor
