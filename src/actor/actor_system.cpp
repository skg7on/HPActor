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

#include <hpactor/actor/actor_type_registry.hpp>
#include <hpactor/actor/ask_manager.hpp>
#include <hpactor/actor/durable/in_memory_state_store.hpp>
#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/actor/http_gateway_actor.hpp>
#include <hpactor/actor/lifecycle/passivation_config.hpp>
#include <hpactor/actor/lifecycle/passivation_manager.hpp>
#include <hpactor/actor/lifecycle/shutdown_coordinator.hpp>
#include <hpactor/actor/local_actor.hpp>
#include <hpactor/actor/spawn_receiver.hpp>
#include <hpactor/config/actor_factory_registry.hpp>
#include <hpactor/config/toml_parser.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/fault/fault_macros.hpp>
#include <hpactor/hpactor_config.hpp>
#include <hpactor/mailbox/backpressure_coordinator.hpp>
#include <hpactor/mailbox/delivery_pipeline.hpp>
#include <hpactor/mailbox/local_delivery_engine.hpp>
#include <hpactor/msg/outbound_delivery_tracker.hpp>
#include <hpactor/process/process_manager.hpp>

#include <chrono>
#include <mutex>
#include <thread>

#include <hpactor/actor/spawn.hpp>
#include <hpactor/cli/cli_actor.hpp>
#include <hpactor/core/actor_system_ids.hpp>
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

#include <hpactor/common.pb.h>
#include <hpactor/messages.pb.h>

namespace hpactor {

// -----------------------------------------------------------------------------
// actor_registry implementation
// -----------------------------------------------------------------------------
actor_registry::actor_registry(EndPoint endpoint) : endpoint_(endpoint) {}

void actor_registry::put(const std::string& name, ActorAddress addr) {
    actors_[name] = addr;
}

ActorAddress actor_registry::get(const std::string& name) const {
    auto it = actors_.find(name);
    if (it != actors_.end()) {
        return it->second;
    }
    return invalid_actor_addr;
}

void actor_registry::erase(const std::string& name) {
    actors_.erase(name);
}

// -----------------------------------------------------------------------------
// ActorSystem implementation
// -----------------------------------------------------------------------------
ActorSystem::ActorSystem(const Config& config)
    : config_(config), endpoint_(config.endpoint), registry_(endpoint_),
      start_time_(std::chrono::steady_clock::now()),
      scheduler_(std::make_unique<sched::HybridScheduler>(
          *this, config.scheduler_threads, 4, config.timer_backend,
          config.scheduler_start_paused)),
      actor_type_registry_(std::make_unique<ActorTypeRegistry>()) {
    // ── System protobuf types ──────────────────────────────────────────
    proto_registry_.register_system_types();

    // ── Dead-letter queue ──────────────────────────────────────────────
    dead_letters_ =
        std::make_unique<mailbox::DeadLetterQueue>(config_.dead_letters);

    // ── Receiver dedup cache ───────────────────────────────────────────
    dedup_cache_ = std::make_unique<adt::DedupCache>(adt::DedupCache::Config{});

    // ── Extracted runtime components ───────────────────────────────────
    local_delivery_engine_ =
        std::make_unique<LocalDeliveryEngine>(actor_directory_);
    {
        BackpressureCoordinator::Config bp_cfg;
        bp_cfg.metrics_ring_buffer = nullptr;
        bp_cfg.transport = nullptr;
        bp_cfg.actor_directory = &actor_directory_;
        bp_cfg.endpoint = endpoint_;
        backpressure_coordinator_ =
            std::make_unique<BackpressureCoordinator>(std::move(bp_cfg));
    }

    // ── Outbound delivery tracker ─────────────────────────────────────
    outbound_tracker_ = std::make_unique<msg::OutboundDeliveryTracker>();

    // ── Delivery pipeline ──────────────────────────────────────────────
    // MUST be created before the scheduler starts so that early-boot
    // actor spawns can deliver messages through it.
    {
        mailbox::DeliveryPipeline::Config pipeline_cfg;
        pipeline_cfg.dlq = dead_letters_.get();
        pipeline_cfg.outbound_tracker = outbound_tracker_.get();
        pipeline_cfg.metrics = nullptr;
        pipeline_cfg.dedup_cache = dedup_cache_.get();
        pipeline_cfg.endpoint = endpoint_;
        pipeline_cfg.default_message_ttl_ms = config_.default_message_ttl_ms;

        pipeline_cfg.get_actor = [this](ActorId id) {
            auto entry = actor_directory_.find(id);
            return entry.has_value() ? entry->instance : nullptr;
        };
        pipeline_cfg.get_mailbox = [this](ActorId id) {
            auto mailbox = actor_directory_.find_mailbox(id);
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

        delivery_pipeline_ =
            std::make_unique<mailbox::DeliveryPipeline>(std::move(pipeline_cfg));
    }

    // ── Metrics subsystem ──────────────────────────────────────────────
    if (metrics_config_.enabled) {
        metrics_ring_buffer_ =
            std::make_shared<metrics::MpscRingBuffer<metrics::MetricEvent>>();
        scheduler_->set_metrics_ring_buffer(metrics_ring_buffer_.get());
        delivery_pipeline_->set_metrics(metrics_ring_buffer_.get());
        backpressure_coordinator_->set_metrics_ring_buffer(
            metrics_ring_buffer_.get());

        auto m_actor = spawn<metrics::MetricsActor>(metrics_ring_buffer_);
        metrics_actor_ = static_cast<metrics::MetricsActor*>(m_actor.get().get());
    }

    // ── Logging subsystem ──────────────────────────────────────────────
    if (logging_config_.enabled) {
        log_manager_ = std::make_unique<log::LogManager>(logging_config_);
        log_manager_->start();
        logger_ = &log_manager_->logger();
    }

    if (logger_) [[unlikely]] {
        scheduler_->set_logger(logger_);
    }

    fault_controller_.set_log_manager(log_manager_.get());

    // Initialize process-mode subystem (daemonize, pidfile, signal handling).
    // Must happen before scheduler_->start() so daemonization forks before
    // any worker threads are created.
    (void)process::ProcessManager::init(config_.process);

    scheduler_->start();

    apply_tracing_config(config_.tracing);

    if (config.enable_network) {
        network_loop_ = std::make_unique<net::EventLoop>();
        network_loop_->set_actor_system(this);

        if (config.service_discovery) {
            discovery_ = config.service_discovery;
        } else if (config.registrar.udp_port > 0) {
            auto reg = std::make_shared<net::UdpRegistrar>(
                config.registrar, endpoint_, network_loop_.get());
            discovery_ = reg;
            registrar_ = reg;
        } else {
            discovery_ =
                std::make_shared<net::StaticDiscovery>(std::vector<net::Member>{});
        }

        discovery_->start();

        discovery_->on_member_change([this](const net::Member& m, bool joined) {
            if (!joined) {
                on_node_dead(m.identity.endpoint);
            }
        });

        location_cache_ = std::make_shared<net::ActorLocationCache>();
        if (network_loop_) {
            cache_purge_timer_ = network_loop_->run_every(
                [this]() {
                    if (location_cache_)
                        location_cache_->purge_expired();
                },
                60000);
        }

        // Periodic retry processing for at-least-once delivery.
        if (outbound_tracker_ && network_loop_) {
            retry_timer_ = network_loop_->run_every(
                [this]() {
                    if (outbound_tracker_) {
                        uint64_t now_ns = static_cast<uint64_t>(
                            std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count());
                        outbound_tracker_->process_retries(
                            now_ns,
                            [](const msg::OutboundDeliveryTracker::PendingSend&) {
                                // Resend via transport (implemented in
                                // follow-up).
                            });
                    }
                },
                100); // poll every 100ms
        }

        transport_ = std::make_unique<net::TcpTransport>(endpoint_, config.tls,
                                                         config.pool, nullptr);
        backpressure_coordinator_->set_transport(transport_.get());

        if (metrics_ring_buffer_) {
            transport_->set_metrics_ring_buffer(metrics_ring_buffer_.get());
        }

        rpc_channel_ = std::make_unique<RpcChannel>(
            transport_.get(), scheduler_.get(), config_.default_ask_max_retries);

        ask_manager_ = std::make_unique<AskManager>(scheduler_.get(), this);

        if (config_.enable_http_client) {
            http_client_ = std::make_unique<net::HttpClient>(network_loop_.get());
        }

        if (config_.enable_http_gateway) {
            http_gateway_actor_ = spawn<net::HTTPGatewayActor>(
                config_.http_bind_host, config_.http_port);
        }

        if (config.tcp_port > 0) {
            transport_->set_rpc_handler(
                [this](const hpactor::RpcResponseFrame& response) {
                    rpc_channel_->on_response(response);
                });
            transport_->set_actor_message_handler([this](const net::WireFrame& frame) {
                this->deliver_remote(frame);
            });
            transport_->listen(config.tcp_port);
        }

        network_thread_ = std::thread([this]() {
            while (network_loop_->wait(100) >= 0) {
                network_loop_->process_completions();
                if (!is_running())
                    break;
            }
        });

        auto spawn_receiver = std::make_shared<SpawnReceiver>(
            *this, *actor_type_registry_, transport_.get());
        spawn_receiver->set_address(
            ActorAddress{endpoint_, SystemActorType, SpawnReceiverId, 0});

        auto spawn_mailbox =
            std::make_shared<mailbox::MPSCActorMailbox<TypedMessage>>(
                SpawnReceiverId, scheduler_.get(), mailbox_config_for_spawn());
        auto spawn_ctx =
            std::make_shared<ActorContext>(Actor(spawn_receiver), this);
        spawn_receiver->set_context(spawn_ctx.get());

        ActorDirectoryEntry spawn_entry;
        spawn_entry.actor = Actor(spawn_receiver);
        spawn_entry.instance = spawn_receiver;
        spawn_entry.mailbox = spawn_mailbox;
        spawn_entry.context = spawn_ctx;
        actor_directory_.insert(std::move(spawn_entry));
    }

    {
        auto durable_store = std::make_unique<InMemoryStateStore>();
        PassivationConfig defaults;
        passivation_manager_ = std::make_unique<PassivationManager>(
            *this, durable_store.release(), defaults);
    }

    if (config_.cli.enabled) {
        auto spawned = spawn<cli::CliActor>(config_.cli);
        cli_actor_ = std::static_pointer_cast<cli::CliActor>(spawned.get());
    }

    fault_controller_.install();

    shutdown_coordinator_ =
        std::make_unique<ShutdownCoordinator>(ShutdownCoordinatorDependencies{
            .phase = &shutdown_phase_,
            .running = &running_,
            .set_ready =
                [this](bool ready) {
                    is_ready_.store(ready, std::memory_order_release);
                },
            .actor_snapshot =
                [this]() {
                    auto entries = actor_directory_.snapshot();
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
                auto mailbox = actor_directory_.find_mailbox(id);
                return static_cast<void*>(mailbox.get());
            },
            .stop_remote_runtime =
                [this]() {
                    if (network_loop_) {
                        network_loop_->stop();
                    }
                },
            .leave_discovery = []() {},
            .flush_telemetry =
                []() {
                    // Metrics ring buffer is NOT destroyed here — scheduler
                    // workers hold raw pointers to it and may still be running.
                    // The ActorSystem destructor cleans it up after
                    // scheduler_->stop() ensures all workers have exited.
                },
        });
}

ActorSystem::~ActorSystem() {
    running_.store(false);
    if (config_.enable_network) {
        if (network_loop_) {
            network_loop_->stop();
        }
        if (network_thread_.joinable()) {
            network_thread_.join();
        }
        if (transport_) {
            transport_->stop_listening();
        }
        if (discovery_) {
            discovery_->stop();
        }
    }
    if (log_manager_) {
        log_manager_->stop();
    }
    if (trace_manager_) {
        trace_manager_->stop();
    }
    scheduler_->stop();
    fault_controller_.remove();
}

void ActorSystem::apply_tracing_config(const tracing::TraceConfig& config) {
    tracing_config_ = config;
    if (!tracing_config_.enabled) {
        if (trace_manager_) {
            trace_manager_->stop();
            trace_manager_.reset();
        }
        return;
    }
    trace_manager_ = std::make_unique<tracing::TraceManager>(tracing_config_, this);
    trace_manager_->start();
}

void ActorSystem::on_node_dead(EndPoint dead_ep) {
    auto entries = actor_directory_.snapshot();
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
    if (location_cache_)
        location_cache_->evict_node(dead_ep);
}

// ── Backpressure API (logging wrappers around BackpressureCoordinator) ───────

void ActorSystem::signal_backpressure(const mailbox::BackpressureSignal& signal) {
    backpressure_coordinator_->deliver_to_sender(signal);
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
    backpressure_coordinator_->emit_local_signal(signal, state);
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
    backpressure_coordinator_->emit_remote_signal(signal, state);
    if (!transport_ && !backpressure_signal_wire_sink_for_test_) {
        HPACTOR_LOG_WARNING(log::LogCategory::kMailbox, signal.target.id, 0,
                            "backpressure_signal_remote_send_failed",
                            log::field("sender", signal.sender.id.value()));
    }
}

bool ActorSystem::handle_remote_backpressure_signal(const net::WireFrame& frame) {
    return backpressure_coordinator_->handle_remote_signal(frame);
}

void ActorSystem::set_backpressure_signal_wire_sink_for_test(
    BackpressureSignalWireSink sink) {
    backpressure_coordinator_->set_wire_sink_for_test(std::move(sink));
}

// ── Actor registry ──────────────────────────────────────────────────────────

void ActorSystem::register_actor(const std::string& name, Actor actor) {
    registry_.put(name, actor.address());
    actor_directory_.register_name(name, actor.address());
}

Actor ActorSystem::resolve_actor(const std::string& name) {
    auto actor_opt = actor_directory_.resolve_actor(name);
    if (actor_opt.has_value()) {
        return actor_opt.value();
    }
    return Actor{};
}

void ActorSystem::unregister_actor(const std::string& name) {
    registry_.erase(name);
}

void ActorSystem::register_actor_type(const ActorTypeDef& def) {
    actor_types_[def.id] = def;
}

ActorTypeDef ActorSystem::get_actor_type(ActorType type) const {
    auto it = actor_types_.find(type);
    if (it != actor_types_.end()) {
        return it->second;
    }
    return ActorTypeDef{};
}

std::shared_ptr<AbstractActor> ActorSystem::get_actor(ActorId id) {
    auto entry = actor_directory_.find(id);
    if (entry.has_value()) {
        return entry->instance;
    }
    return nullptr;
}

mailbox::MPSCActorMailbox<TypedMessage>* ActorSystem::get_mailbox(ActorId id) {
    auto mailbox = actor_directory_.find_mailbox(id);
    return mailbox.get();
}

size_t ActorSystem::actor_count() const {
    return actor_directory_.size();
}

void ActorSystem::for_each_actor(
    std::function<void(ActorId, AbstractActor&)> callback) const {
    auto entries = actor_directory_.snapshot();
    for (const auto& entry : entries) {
        callback(entry.actor.id(), *entry.instance);
    }
}

cli::CliActor* ActorSystem::cli_actor() const {
    return cli_actor_.get();
}

metrics::MetricsActor* ActorSystem::metrics_actor() const {
    return metrics_actor_;
}

// ── Dead-letter queue ───────────────────────────────────────────────────────

bool ActorSystem::dead_letter(mailbox::DeadLetterRecord record) noexcept {
    if (!dead_letters_) {
        return false;
    }
    return dead_letters_->try_push(std::move(record));
}

mailbox::DeadLetterQueueSnapshot ActorSystem::dead_letter_snapshot() const noexcept {
    if (!dead_letters_) {
        return {};
    }
    return dead_letters_->snapshot();
}

bool ActorSystem::pop_dead_letter(mailbox::DeadLetterRecord& out) noexcept {
    if (!dead_letters_) {
        return false;
    }
    return dead_letters_->try_pop(out);
}

// ── Mailbox config helpers ──────────────────────────────────────────────────

mailbox::MailboxConfig ActorSystem::mailbox_config_for_spawn() const {
    mailbox::MailboxConfig cfg;
    cfg.capacity.max_messages = config_.mailbox.default_capacity;
    cfg.capacity.max_bytes = config_.mailbox.default_byte_capacity;
    cfg.overflow_policy = config_.mailbox.default_policy;
    cfg.high_watermark = config_.mailbox.high_watermark;
    cfg.low_watermark = config_.mailbox.low_watermark;
    cfg.critical_watermark = config_.mailbox.critical_watermark;
    cfg.priority_aware = config_.mailbox.priority_aware;
    cfg.priority_levels = config_.mailbox.priority_levels;
    cfg.protected_system_messages = config_.mailbox.protected_system_messages;
    cfg.max_overflow_depth = config_.mailbox.max_overflow_depth;
    cfg.signal_min_interval_ms = config_.mailbox.signal_min_interval_ms;
    cfg.backpressure_mode = config_.mailbox.backpressure_mode;
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
    return delivery_pipeline_->try_deliver(target, std::move(msg), priority,
                                           deadline_ns, options);
}

mailbox::DeliveryResult
ActorSystem::deliver_with_result(ActorId target, TypedMessage msg,
                                 uint8_t priority, int64_t deadline_ns,
                                 mailbox::DeliveryOptions options) {
    return delivery_pipeline_->deliver_with_result(
        target, std::move(msg), priority, deadline_ns, options);
}

void ActorSystem::deliver_local(ActorId target, TypedMessage msg) {
    if (!delivery_pipeline_)
        return;
    (void)delivery_pipeline_->try_deliver(target, std::move(msg));
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
    (void)delivery_pipeline_->try_deliver(target, std::move(msg), priority,
                                          deadline_ns, {});
}

void ActorSystem::deliver_remote(const net::WireFrame& frame) {
    // ── ACK/NACK control frame dispatch ────────────────────────────────
    constexpr uint32_t kControlAck = 1 << 5;
    constexpr uint32_t kControlNack = 1 << 6;
    uint32_t flags = frame.pb_frame.flags();

    if ((flags & kControlAck) && outbound_tracker_) {
        outbound_tracker_->on_ack(MessageId{frame.pb_frame.message_id()},
                                  net::from_proto(frame.pb_frame.sender()).endpoint);
        return;
    }
    if ((flags & kControlNack) && outbound_tracker_) {
        uint32_t reason_code = frame.pb_frame.type_tag();
        uint32_t retry_after_ms = 0;
        if (frame.pb_frame.payload().size() >= sizeof(uint32_t)) {
            std::memcpy(&retry_after_ms, frame.pb_frame.payload().data(),
                        sizeof(uint32_t));
        }
        outbound_tracker_->on_nack(MessageId{frame.pb_frame.message_id()},
                                   net::from_proto(frame.pb_frame.sender()).endpoint,
                                   reason_code, retry_after_ms);
        return;
    }

    if (static_cast<TypeTag>(frame.pb_frame.type_tag()) ==
        TypeTag::BackpressureSignalTag) {
        (void)backpressure_coordinator_->handle_remote_signal(frame);
        return;
    }
    StreamBuffer payload(frame.pb_frame.payload().begin(),
                         frame.pb_frame.payload().end());
    TypedMessage msg(static_cast<TypeTag>(frame.pb_frame.type_tag()),
                     std::move(payload));
    msg.set_sender_address(net::from_proto(frame.pb_frame.sender()));
    if (frame.pb_frame.has_trace_context()) {
        uint16_t max_state = tracing_config_.max_tracestate_len;
        auto parsed = net::trace_context_from_proto(
            frame.pb_frame.trace_context(), max_state);
        if (parsed.has_value()) {
            msg.set_trace_context(parsed.value());
        }
    }
    deliver_local(net::from_proto(frame.pb_frame.receiver()).id, std::move(msg));
}

void ActorSystem::enqueue_completion(net::OpCompletion completion) {
    StreamBuffer payload(sizeof(net::OpCompletion));
    std::memcpy(payload.data(), &completion, sizeof(net::OpCompletion));
    TypedMessage msg(TypeTag::IoCompletionTag, std::move(payload));
    deliver_local(completion.actor, std::move(msg));
}

net::Transport* ActorSystem::get_transport_for(const EndPoint& /*endpoint*/) {
    if (!config_.enable_network) {
        return nullptr;
    }
    return transport_.get();
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

    if (!config_.enable_network || !rpc_channel_) {
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
    net::to_proto(pb_req.mutable_supervisor(), system_actor_.address());

    StreamBuffer request_bytes = proto_registry_.serialize(pb_req);

    auto timeout_ms = timeout_override.is_default() ? config_.spawn_timeout_ms
                                                    : timeout_override.value;

    ActorAddress target{remote_endpoint, SystemActorType, SpawnReceiverId, 0};

    auto rpc_future = rpc_channel_->call_raw(target, request_bytes, timeout_ms);

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

// ── spawn_configured ────────────────────────────────────────────────────────

Actor ActorSystem::spawn_configured(std::shared_ptr<AbstractActor> actor,
                                    const config::ActorDef& def) {
    FAULT_INJECT("hpactor.actor.spawn.fail") {
        return {};
    }
    ActorId id = actor_directory_.allocate_id();
    actor->set_address(ActorAddress(endpoint_, actor->type(), id, 0));
    actor->set_type_name(def.behavior);

    auto mailbox_ptr = std::make_shared<mailbox::MPSCActorMailbox<TypedMessage>>(
        id, scheduler_.get(), mailbox_config_for_actor_def(def));
    auto* mbox = mailbox_ptr.get();

    auto* local = static_cast<LocalActor*>(actor.get());
    auto actor_ctx = std::make_shared<ActorContext>(Actor(actor), this);
    local->set_context(actor_ctx.get());

    ActorDirectoryEntry entry;
    entry.actor = Actor(actor);
    entry.instance = actor;
    entry.mailbox = mailbox_ptr;
    entry.context = actor_ctx;
    actor_directory_.insert(std::move(entry));

    actor->set_scheduler(scheduler_.get());
    actor->set_mailbox(mbox);

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
            scheduler_->notify_ready(id, 0, INT64_MAX);
            break;
        case sched::DispatchPolicy::DedicatedThread:
            scheduler_->register_dedicated_thread(id, hints.cpu_affinity);
            break;
        case sched::DispatchPolicy::DedicatedPool:
            scheduler_->register_dedicated_pool(id, hints.pool_size);
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
        metrics_config_.enabled = model.system.metrics_enabled;
        metrics_config_.ring_buffer_capacity =
            model.system.metrics_ring_buffer_capacity;
        metrics_config_.metrics_path = model.system.metrics_path;
    }

    logging_config_ = model.system.logging;

#define HPACTOR_SYSTEM_TOML_FIELD(name, type, toml, def)                       \
    config_.name = static_cast<decltype(config_.name)>(model.system.name);
#include <hpactor/config/system_toml_fields.def>
#undef HPACTOR_SYSTEM_TOML_FIELD

#define HPACTOR_MAILBOX_FIELD(name, type, toml, def)                           \
    config_.mailbox.name = model.system.mailbox.name;
#include <hpactor/config/mailbox_fields.def>
#undef HPACTOR_MAILBOX_FIELD

    config_.dead_letters = model.system.dead_letters;
    dead_letters_ =
        std::make_unique<mailbox::DeadLetterQueue>(config_.dead_letters);

    apply_tracing_config(model.system.tracing);

    // Sync process config from TOML (pidfile, watchdog, working directory).
    // Daemonization must have already occurred via the constructor path;
    // this updates in-memory config for notification and signal handling.
    config_.process = model.system.process;

    config_.pool.outbound_limits = model.system.transport_outbound_limits;
    config_.pool.circuit_breaker_cfg = model.system.transport_circuit_breaker;
    if (transport_) {
        transport_->set_pool_config(config_.pool);
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
        mem::MemStdAllocator<ActorId>(system_actor_.id(),
                                      mem::RegionType::kInternal));
    for (const auto& actor_def : model.actors) {
        auto factory = registry.get_factory(actor_def.behavior);
        auto actor_ptr = factory(nullptr, *this);

        Actor actor_handle = spawn_configured(std::move(actor_ptr), actor_def);

        registry_.put(actor_def.id, actor_handle.address());

        spawned_ids.push_back(actor_handle.id());
    }

    ActorAddress sys_addr = system_actor_.address();
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
    shutdown_coordinator_->execute(opts);
    return result<void>::make();
}

ShutdownPhase ActorSystem::shutdown_phase() const noexcept {
    return shutdown_phase_.load(std::memory_order_acquire);
}

bool ActorSystem::is_ready() const noexcept {
    return is_ready_.load(std::memory_order_acquire);
}

bool ActorSystem::is_draining() const noexcept {
    return shutdown_phase_.load(std::memory_order_acquire) ==
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

} // namespace hpactor
