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

#include <hpactor/actor/ask_manager.hpp>
#include <hpactor/actor/backpressure_coordinator.hpp>
#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/actor/http_gateway_actor.hpp>
#include <hpactor/actor/local_actor.hpp>
#include <hpactor/actor/local_delivery_engine.hpp>
#include <hpactor/actor/shutdown_coordinator.hpp>
#include <hpactor/actor/spawn_receiver.hpp>
#include <hpactor/actor_type_registry.hpp>
#include <hpactor/config/actor_factory_registry.hpp>
#include <hpactor/config/toml_parser.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/fault/fault_macros.hpp>
#include <hpactor/hpactor_config.hpp>

#include <chrono>

#include <hpactor/cli/cli_actor.hpp>
#include <hpactor/core/actor_system_ids.hpp>
#include <hpactor/log/log_manager.hpp>
#include <hpactor/log/logger.hpp>
#include <hpactor/mailbox/backpressure_signal_serialization.hpp>
#include <hpactor/mem/std_allocator.hpp>
#include <hpactor/net/async_io_fwd.hpp>
#include <hpactor/net/frame.hpp>
#include <hpactor/net/tcp_transport.hpp>
#include <hpactor/sched/scheduler.hpp>
#include <hpactor/spawn.hpp>
#include <hpactor/types/failure_envelope.hpp>

// Protobuf message types for spawn serialization
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
      scheduler_(std::make_unique<sched::HybridScheduler>(
          *this, config.scheduler_threads, 4, config.timer_backend,
          config.scheduler_start_paused)),
      actor_type_registry_(std::make_unique<ActorTypeRegistry>()) {
    // Register system protobuf types
    proto_registry_.register_system_types();

    // Initialize dead-letter queue
    dead_letters_ =
        std::make_unique<mailbox::DeadLetterQueue>(config_.dead_letters);

    // Initialize receiver dedup cache for at-least-once delivery
    dedup_cache_ =
        std::make_unique<mailbox::DedupCache>(mailbox::DedupCache::Config{});

    // Initialize extracted runtime components
    local_delivery_engine_ =
        std::make_unique<LocalDeliveryEngine>(actor_directory_);
    backpressure_coordinator_ = std::make_unique<BackpressureCoordinator>(*this);

    // Initialize metrics subsystem (before scheduler so instrumentation is
    // ready)
    if (metrics_config_.enabled) {
        metrics_ring_buffer_ =
            std::make_shared<metrics::MpscRingBuffer<metrics::MetricEvent>>();
        scheduler_->set_metrics_ring_buffer(metrics_ring_buffer_.get());
    }

    // Initialize logging subsystem before starting the scheduler so that
    // worker threads see a valid global logger, not a dangling pointer
    // left over from a previous ActorSystem instance.
    if (logging_config_.enabled) {
        log_manager_ = std::make_unique<log::LogManager>(logging_config_);
        log_manager_->start();
        logger_ = &log_manager_->logger();
    }

    // Wire logger to scheduler for scheduler-event logs
    if (logger_) [[unlikely]] {
        scheduler_->set_logger(logger_);
    }

    fault_controller_.set_log_manager(log_manager_.get());

    scheduler_->start();

    apply_tracing_config(config_.tracing);

    if (config.enable_network) {
        network_loop_ = std::make_unique<net::EventLoop>();
        network_loop_->set_actor_system(this);

        // ── Service discovery backend ────────────────────────────
        if (config.service_discovery) {
            discovery_ = config.service_discovery;
        } else if (config.registrar.udp_port > 0) {
            auto reg = std::make_shared<net::UdpRegistrar>(
                config.registrar, endpoint_, network_loop_.get());
            discovery_ = reg;
            registrar_ = reg; // shared ownership for registrar() accessor
        } else {
            discovery_ =
                std::make_shared<net::StaticDiscovery>(std::vector<net::Member>{});
        }

        discovery_->start();

        discovery_->on_member_change([this](const net::Member& m, bool joined) {
            if (!joined) {
                on_node_dead(m.identity.endpoint);
            }
            // Note: proactive connection pool warming (prewarm_pool) will be
            // integrated in a follow-up task when ConnectionPool is updated.
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

        transport_ = std::make_unique<net::TcpTransport>(endpoint_, config.tls,
                                                         config.pool, nullptr);

        // Propagate metrics ring buffer to transport for connection pool
        // metrics
        if (metrics_ring_buffer_) {
            transport_->set_metrics_ring_buffer(metrics_ring_buffer_.get());
        }

        rpc_channel_ = std::make_unique<RpcChannel>(
            transport_.get(), scheduler_.get(), config_.default_ask_max_retries);

        // Create AskManager for local ask() request tracking
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

        {
            std::lock_guard<std::mutex> lock(actors_mutex_);
            actors_.emplace(SpawnReceiverId, spawn_receiver);
        }

        {
            std::lock_guard<std::mutex> lock(mailboxes_mutex_);
            mailboxes_.emplace(
                SpawnReceiverId,
                std::make_unique<mailbox::MPSCActorMailbox<TypedMessage>>(
                    SpawnReceiverId, scheduler_.get(), mailbox_config_for_spawn()));
        }
    }

    // Spawn CLI actor (runtime opt-in via config_.cli.enabled)
    if (config_.cli.enabled) {
        auto spawned = spawn<cli::CliActor>(config_.cli);
        cli_actor_ = std::static_pointer_cast<cli::CliActor>(spawned.get());
    }

    fault_controller_.install();

    // Initialize extracted runtime components
    shutdown_coordinator_ =
        std::make_unique<ShutdownCoordinator>(ShutdownCoordinatorDependencies{
            .phase = &shutdown_phase_,
            .set_ready =
                [this](bool ready) {
                    is_ready_.store(ready, std::memory_order_release);
                },
            .actor_snapshot = [this]() -> std::vector<ActorId> {
                std::lock_guard<std::mutex> lock(actors_mutex_);
                std::vector<ActorId> ids;
                ids.reserve(actors_.size());
                for (const auto& [id, _] : actors_) {
                    (void)_;
                    ids.push_back(id);
                }
                return ids;
            },
            .request_actor_drain =
                [](ActorId id) {
                    // Drain requests are sent via message passing
                    // Full integration in follow-up task
                    (void)id;
                },
            .actors_drained = []() -> bool { return true; },
            .stop_remote_runtime =
                [this]() {
                    if (network_loop_) {
                        network_loop_->stop();
                    }
                },
            .leave_discovery =
                []() {
                    // Discovery stop handled by existing shutdown path
                },
            .flush_telemetry =
                [this]() {
                    if (metrics_ring_buffer_) {
                        metrics_ring_buffer_.reset();
                    }
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
    // Find all actors linked to or monitoring actors on the dead endpoint.
    // Uses the internal actor_contexts_ map directly (actor_context() is
    // a protected member of AbstractActor, not accessible from ActorSystem).
    std::lock_guard<std::mutex> lock(actor_contexts_mutex_);
    for (const auto& [id, ctx] : actor_contexts_) {
        if (!ctx)
            continue;
        for (const auto& addr : ctx->linked_actors()) {
            if (addr.endpoint == dead_ep) {
                TypedMessage down(TypeTag::DownMsg, StreamBuffer{});
                down.set_sender_address(ActorAddress{dead_ep, 0, ActorId(0), 0});
                deliver_local(id, std::move(down));
                break;
            }
        }
    }
    if (location_cache_)
        location_cache_->evict_node(dead_ep);
}

void ActorSystem::signal_backpressure(const mailbox::BackpressureSignal& signal) {
    if (signal.sender.id == ActorId{0})
        return; // no sender

    std::lock_guard<std::mutex> lock(actor_contexts_mutex_);
    auto it = actor_contexts_.find(signal.sender.id);
    if (it != actor_contexts_.end() && it->second) {
        it->second->handle_backpressure(signal);
    }
}

namespace {

bool local_signal_enabled(mailbox::BackpressureMode mode) noexcept {
    return mode == mailbox::BackpressureMode::LocalSignal ||
           mode == mailbox::BackpressureMode::LocalAndRemoteSignal;
}

bool remote_signal_enabled(mailbox::BackpressureMode mode) noexcept {
    return mode == mailbox::BackpressureMode::RemoteSignal ||
           mode == mailbox::BackpressureMode::LocalAndRemoteSignal;
}

bool pressure_result_should_signal(const mailbox::EnqueueResult& result) noexcept {
    if (result.code == mailbox::EnqueueResultCode::AcceptedWithSoftPressure) {
        return true;
    }
    return !result.accepted() && result.retryable();
}

} // namespace

void ActorSystem::maybe_emit_backpressure_signal(
    mailbox::MPSCActorMailbox<TypedMessage>* mailbox,
    const mailbox::EnqueueResult& result, const mailbox::MailboxEnvelopeMeta& meta,
    bool emit_requested, mailbox::BackpressureMode backpressure_mode) {
    if (!emit_requested || mailbox == nullptr ||
        !pressure_result_should_signal(result)) {
        return;
    }

    const auto mode = backpressure_mode;
    const bool sender_is_remote =
        meta.sender.endpoint != endpoint_ && meta.sender.id != ActorId{0};

    if (sender_is_remote && !remote_signal_enabled(mode)) {
        return;
    }
    if (!sender_is_remote && !local_signal_enabled(mode)) {
        return;
    }

    const uint64_t now_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    // Force past the rate limiter when the message was rejected — the sender
    // must always be notified of dropped messages regardless of how recently
    // a soft-pressure warning was issued.
    const bool is_rejection = !result.accepted();
    auto sequence = mailbox->try_acquire_backpressure_signal(
        now_ns, result.pressure_state, is_rejection);
    if (!sequence.has_value()) {
        return;
    }

    mailbox::BackpressureSignal signal;
    signal.target = ActorAddress{endpoint_, ActorType{0}, result.target, 0};
    signal.sender = meta.sender;
    signal.reason = result.pressure_reason;
    signal.depth = result.depth;
    signal.capacity = result.capacity;
    signal.bytes = result.bytes;
    signal.byte_capacity = result.byte_capacity;
    signal.pressure_ratio = result.pressure_ratio;
    signal.retry_after = result.retry_after;
    signal.sequence = sequence.value();

    if (sender_is_remote) {
        emit_remote_backpressure_signal(signal, result.pressure_state);
    } else {
        emit_local_backpressure_signal(signal, result.pressure_state);
    }
}

void ActorSystem::emit_local_backpressure_signal(
    const mailbox::BackpressureSignal& signal, mailbox::MailboxPressureState state) {
    if (metrics_ring_buffer_) {
        metrics::MetricEvent evt{};
        evt.actor_id = signal.target.id;
        evt.event_type = metrics::MetricEventType::kBackpressureSignal;
        evt.code = static_cast<uint8_t>(signal.reason);
        evt.aux = static_cast<uint8_t>(state);
        evt.value_hi = 1;
        metrics_ring_buffer_->try_push(evt);
    }

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

    signal_backpressure(signal);
}

void ActorSystem::emit_remote_backpressure_signal(
    const mailbox::BackpressureSignal& signal, mailbox::MailboxPressureState state) {
    auto payload = mailbox::serialize_backpressure_signal(signal, state);
    if (payload.empty()) {
        return;
    }

    // Emit metrics unconditionally — the signal was generated and we need
    // observability regardless of whether the wire send succeeds.
    if (metrics_ring_buffer_) {
        metrics::MetricEvent evt{};
        evt.actor_id = signal.target.id;
        evt.event_type = metrics::MetricEventType::kBackpressureSignal;
        evt.code = static_cast<uint8_t>(signal.reason);
        evt.aux = static_cast<uint8_t>(state);
        evt.value_hi = 1;
        metrics_ring_buffer_->try_push(evt);
    }

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

    net::WireFrame frame;
    net::to_proto(frame.pb_frame.mutable_sender(), signal.target);
    net::to_proto(frame.pb_frame.mutable_receiver(), signal.sender);
    frame.pb_frame.set_type_tag(
        static_cast<uint32_t>(TypeTag::BackpressureSignalTag));
    frame.pb_frame.set_message_id(signal.sequence);
    frame.pb_frame.set_payload(reinterpret_cast<const char*>(payload.data()),
                               payload.size());

    TransportSendResult sent = TransportSendResult::NotConnected;
    auto encoded = frame.encode();
    if (backpressure_signal_wire_sink_for_test_) {
        sent = backpressure_signal_wire_sink_for_test_(signal.sender, encoded)
                   ? TransportSendResult::Sent
                   : TransportSendResult::WriteError;
    } else if (transport_) {
        sent = transport_->try_send(signal.sender, encoded);
    }

    if (sent != TransportSendResult::Sent) {
        HPACTOR_LOG_WARNING(log::LogCategory::kMailbox, signal.target.id, 0,
                            "backpressure_signal_remote_send_failed",
                            log::field("sender", signal.sender.id.value()));
    }
}

bool ActorSystem::handle_remote_backpressure_signal(const net::WireFrame& frame) {
    StreamBuffer payload(frame.pb_frame.payload().begin(),
                         frame.pb_frame.payload().end());
    auto decoded = mailbox::deserialize_backpressure_signal(payload);
    if (!decoded.has_value()) {
        return false;
    }
    signal_backpressure(decoded->signal);
    return true;
}

void ActorSystem::set_backpressure_signal_wire_sink_for_test(
    BackpressureSignalWireSink sink) {
    backpressure_signal_wire_sink_for_test_ = std::move(sink);
}

void ActorSystem::register_actor(const std::string& name, Actor actor) {
    registry_.put(name, actor.address());
    actor_directory_.register_name(name, actor.address());
}

Actor ActorSystem::resolve_actor(const std::string& name) {
    ActorAddress addr = registry_.get(name);
    if (!addr) {
        return Actor{};
    }
    std::lock_guard<std::mutex> lock(actors_mutex_);
    auto it = actors_.find(addr.id);
    if (it == actors_.end()) {
        return Actor{};
    }
    return Actor{it->second};
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
    std::lock_guard<std::mutex> lock(actors_mutex_);
    auto it = actors_.find(id);
    if (it != actors_.end()) {
        return it->second;
    }
    return nullptr;
}

mailbox::MPSCActorMailbox<TypedMessage>* ActorSystem::get_mailbox(ActorId id) {
    std::lock_guard<std::mutex> lock(mailboxes_mutex_);
    auto it = mailboxes_.find(id);
    if (it != mailboxes_.end()) {
        return it->second.get();
    }
    return nullptr;
}

size_t ActorSystem::actor_count() const {
    std::lock_guard<std::mutex> lock(actors_mutex_);
    return actors_.size();
}

void ActorSystem::for_each_actor(
    std::function<void(ActorId, AbstractActor&)> callback) const {
    std::lock_guard<std::mutex> lock(actors_mutex_);
    for (auto& [id, actor] : actors_) {
        callback(id, *actor);
    }
}

cli::CliActor* ActorSystem::cli_actor() const {
    return cli_actor_.get();
}

// -----------------------------------------------------------------------------
// Dead-letter queue
// -----------------------------------------------------------------------------
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

// -----------------------------------------------------------------------------
// Mailbox config helpers
// -----------------------------------------------------------------------------
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

// -----------------------------------------------------------------------------
// Delivery pipeline helpers (anonymous namespace)
// -----------------------------------------------------------------------------
namespace {

using MetricBuf = hpactor::metrics::MpscRingBuffer<hpactor::metrics::MetricEvent>;

// Populate the trace fields on a DeadLetterRecord from a TraceContext.
// TraceId and SpanId are stored as big-endian byte arrays (W3C format);
// convert to uint64_t fields for the DLQ record.
void set_dlq_trace_fields(hpactor::mailbox::DeadLetterRecord& dl,
                          const hpactor::TraceContext& tc) noexcept {
    uint64_t hi = 0, lo = 0, sp = 0;
    for (size_t i = 0; i < 8; ++i) {
        hi = (hi << 8) | tc.trace_id.bytes[i];
        lo = (lo << 8) | tc.trace_id.bytes[i + 8];
        sp = (sp << 8) | tc.span_id.bytes[i];
    }
    dl.trace_id_hi = hi;
    dl.trace_id_lo = lo;
    dl.span_id = sp;
}

// Build the EnqueueResult + observability for a missing-actor target.
[[nodiscard]] hpactor::mailbox::EnqueueResult
reject_missing_actor(hpactor::mailbox::DeadLetterQueue* dlq, MetricBuf* metrics,
                     hpactor::EndPoint endpoint, hpactor::ActorId target,
                     const hpactor::TypedMessage& msg,
                     const hpactor::mailbox::DeliveryOptions& options,
                     uint8_t priority, int64_t deadline_ns) {
    if (dlq) {
        hpactor::mailbox::DeadLetterRecord dl;
        dl.reason = hpactor::mailbox::DeadLetterReason::ActorNotFound;
        dl.source = hpactor::mailbox::DeadLetterSource::LocalDelivery;
        dl.sender = msg.sender_address();
        dl.target =
            hpactor::ActorAddress{endpoint, hpactor::ActorType{0}, target, 0};
        dl.type_tag = msg.type_id();
        dl.message_id = options.message_id;
        dl.frame_flags = options.flags;
        dl.priority = priority;
        dl.deadline_ns = deadline_ns;
        dl.payload_sample = msg.payload();
        dl.timestamp_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
        if (msg.has_trace_context()) {
            set_dlq_trace_fields(dl, msg.trace_context());
        }
        (void)dlq->try_push(std::move(dl));
    }

    hpactor::mailbox::EnqueueResult r;
    r.code = hpactor::mailbox::EnqueueResultCode::ActorNotFound;
    r.target = target;
    if (metrics) {
        hpactor::FailureEnvelope env = hpactor::make_failure_envelope(
            hpactor::FailureReason::NoRoute, target, msg.sender_address(),
            hpactor::ActorAddress{endpoint, hpactor::ActorType{0}, target, 0},
            hpactor::MessageId{options.message_id}, hpactor::TraceContext{},
            hpactor::FailureSource::ActorRuntime,
            "target actor not found in registry");
        hpactor::metrics::MetricEvent evt{};
        evt.timestamp_ns = env.timestamp_ns;
        evt.actor_id = target;
        evt.event_type = hpactor::metrics::MetricEventType::kDeliveryFailure;
        evt.code = static_cast<uint8_t>(env.reason);
        evt.value_hi = 1;
        metrics->try_push(evt);
    }
    return r;
}

// If the message is a tracked duplicate, record the metric and return an
// Accepted result.  Returns nullopt when the message is not a duplicate.
[[nodiscard]] std::optional<hpactor::mailbox::EnqueueResult>
try_accept_duplicate(MetricBuf* metrics, hpactor::mailbox::DedupCache* dedup_cache,
                     hpactor::EndPoint endpoint, hpactor::ActorId target,
                     const hpactor::TypedMessage& msg,
                     const hpactor::mailbox::DeliveryOptions& options) {
    if (!hpactor::mailbox::is_tracked_delivery(options.delivery_mode) ||
        !dedup_cache || options.message_id == 0) {
        return std::nullopt;
    }
    hpactor::ActorId sender_id = msg.sender_address().id;
    if (!dedup_cache->is_duplicate(endpoint, sender_id,
                                   hpactor::MessageId{options.message_id})) {
        return std::nullopt;
    }
    if (metrics) {
        uint64_t ts_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
        hpactor::metrics::MetricEvent evt{};
        evt.timestamp_ns = ts_ns;
        evt.actor_id = target;
        evt.event_type = hpactor::metrics::MetricEventType::kDeliveryDuplicate;
        evt.code = static_cast<uint8_t>(hpactor::FailureReason::Duplicate);
        evt.value_hi = 1;
        metrics->try_push(evt);
    }
    return hpactor::mailbox::EnqueueResult{
        hpactor::mailbox::EnqueueResultCode::Accepted, target};
}

// If the message has a delivery deadline and it has already expired,
// record the metric + dead-letter and return a Rejected result.
// Returns nullopt when the deadline has not expired.
[[nodiscard]] std::optional<hpactor::mailbox::EnqueueResult>
try_reject_expired(hpactor::mailbox::DeadLetterQueue* dlq, MetricBuf* metrics,
                   hpactor::EndPoint endpoint, hpactor::ActorId target,
                   const hpactor::TypedMessage& msg,
                   const hpactor::mailbox::DeliveryOptions& options,
                   uint8_t priority, int64_t deadline_ns) {
    if (options.delivery_mode < hpactor::mailbox::DeliveryMode::ObservableBestEffort) {
        return std::nullopt;
    }
    uint64_t now_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    if (!hpactor::mailbox::is_expired(deadline_ns, now_ns)) {
        return std::nullopt;
    }
    if (metrics) {
        hpactor::FailureEnvelope env = hpactor::make_failure_envelope(
            hpactor::FailureReason::Expired, target, msg.sender_address(),
            hpactor::ActorAddress{endpoint, hpactor::ActorType{0}, target, 0},
            hpactor::MessageId{options.message_id}, msg.trace_context(),
            hpactor::FailureSource::ActorRuntime,
            "message deadline expired before enqueue");
        hpactor::metrics::MetricEvent evt{};
        evt.timestamp_ns = env.timestamp_ns;
        evt.actor_id = target;
        evt.event_type = hpactor::metrics::MetricEventType::kDeliveryExpired;
        evt.code = static_cast<uint8_t>(hpactor::FailureReason::Expired);
        evt.value_hi = 1;
        metrics->try_push(evt);
    }
    if (dlq) {
        hpactor::mailbox::DeadLetterRecord dl;
        dl.reason = hpactor::mailbox::DeadLetterReason::Expired;
        dl.source = hpactor::mailbox::DeadLetterSource::LocalDelivery;
        dl.sender = msg.sender_address();
        dl.target =
            hpactor::ActorAddress{endpoint, hpactor::ActorType{0}, target, 0};
        dl.type_tag = msg.type_id();
        dl.message_id = options.message_id;
        dl.frame_flags = options.flags;
        dl.priority = priority;
        dl.deadline_ns = deadline_ns;
        dl.payload_sample = msg.payload();
        dl.timestamp_ns = now_ns;
        if (msg.has_trace_context()) {
            set_dlq_trace_fields(dl, msg.trace_context());
        }
        (void)dlq->try_push(std::move(dl));
    }
    return hpactor::mailbox::EnqueueResult{
        hpactor::mailbox::EnqueueResultCode::Rejected, target};
}

// Emit metrics, logging, and dead-letter dispatch for a rejected enqueue
// result.  Idempotent — does nothing when the result is accepted.
void emit_rejection_observability(
    hpactor::mailbox::DeadLetterQueue* dlq, MetricBuf* metrics,
    hpactor::EndPoint endpoint, hpactor::ActorId target,
    const hpactor::StreamBuffer& msg_payload, const hpactor::TraceContext& msg_trace,
    bool msg_has_trace, const hpactor::mailbox::MailboxEnvelopeMeta& meta,
    const hpactor::mailbox::EnqueueResult& result,
    const hpactor::mailbox::DeliveryOptions& options,
    hpactor::mailbox::OverflowPolicy overflow_policy) {
    if (result.accepted()) {
        return;
    }

    // Dead-letter when the overflow policy mandates it.
    if (dlq && dlq->config().enabled &&
        overflow_policy == hpactor::mailbox::OverflowPolicy::DeadLetter) {
        hpactor::mailbox::DeadLetterRecord dl;
        dl.reason = hpactor::mailbox::DeadLetterReason::OverflowPolicy;
        dl.source = hpactor::mailbox::DeadLetterSource::MailboxAdmission;
        dl.sender = meta.sender;
        dl.target =
            hpactor::ActorAddress{endpoint, hpactor::ActorType{0}, target, 0};
        dl.type_tag = meta.type_tag;
        dl.message_id = meta.message_id;
        dl.frame_flags = meta.flags;
        dl.priority = meta.priority;
        dl.deadline_ns = meta.deadline_ns;
        dl.mailbox_depth = result.depth;
        dl.mailbox_capacity = result.capacity;
        dl.timestamp_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
        dl.payload_sample = msg_payload;
        if (msg_has_trace) {
            set_dlq_trace_fields(dl, msg_trace);
        }
        (void)dlq->try_push(std::move(dl));
    }

    // Failure envelope metric.
    if (metrics) {
        hpactor::FailureEnvelope env = hpactor::make_failure_envelope(
            result.failure_reason(), target, meta.sender,
            hpactor::ActorAddress{endpoint, hpactor::ActorType{0}, target, 0},
            hpactor::MessageId{options.message_id}, hpactor::TraceContext{},
            hpactor::FailureSource::Mailbox, "mailbox admission rejected");
        hpactor::metrics::MetricEvent evt{};
        evt.timestamp_ns = env.timestamp_ns;
        evt.actor_id = target;
        evt.event_type = hpactor::metrics::MetricEventType::kDeliveryFailure;
        evt.code = static_cast<uint8_t>(env.reason);
        evt.value_hi = 1;
        metrics->try_push(evt);
    }

    // Structured log warning.
    HPACTOR_LOG_WARNING(
        hpactor::log::LogCategory::kActor, target, 0, "delivery_failure",
        hpactor::log::field_lit("reason",
                                hpactor::to_string(result.failure_reason())),
        hpactor::log::field(
            "retryable", result.failure_reason() != hpactor::FailureReason::Unknown &&
                             hpactor::retryable(result.failure_reason())),
        hpactor::log::field("depth", static_cast<uint64_t>(result.depth)),
        hpactor::log::field("capacity", static_cast<uint64_t>(result.capacity)));
}

} // namespace

// -----------------------------------------------------------------------------
// try_deliver_local — bounded admission boundary
// -----------------------------------------------------------------------------
mailbox::EnqueueResult
ActorSystem::try_deliver_local(ActorId target, TypedMessage msg,
                               uint8_t priority, int64_t deadline_ns,
                               mailbox::DeliveryOptions options) {
    auto* mailbox = get_mailbox(target);
    if (mailbox == nullptr) {
        return reject_missing_actor(dead_letters_.get(),
                                    metrics_ring_buffer_.get(), endpoint_,
                                    target, msg, options, priority, deadline_ns);
    }

    // ── Circuit breaker admission gate ──────────────────────────
    if (auto actor_ptr = get_actor(target)) {
        if (actor_ptr->is_event_based_actor()) {
            auto* eba = static_cast<EventBasedActor*>(actor_ptr.get());
            if (eba->quarantine_enabled()) {
                auto* cb = eba->circuit_breaker();
                auto now = std::chrono::steady_clock::now();

                if (cb->state == CircuitBreakerState::kOpen) {
                    auto elapsed =
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            now - cb->opened_at);
                    auto cooldown = eba->quarantine_policy().cooldown_period;
                    if (elapsed >= cooldown) {
                        cb->state = CircuitBreakerState::kHalfOpen;
                        cb->half_open_probe_in_flight = true;
                        if (auto* rb = eba->metrics_ring_buffer()) {
                            auto now_ns =
                                std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    now.time_since_epoch())
                                    .count();
                            metrics::MetricEvent evt{};
                            evt.timestamp_ns = static_cast<uint64_t>(now_ns);
                            evt.actor_id = target;
                            evt.event_type =
                                metrics::MetricEventType::kCircuitStateChange;
                            evt.code = static_cast<uint8_t>(
                                CircuitBreakerState::kHalfOpen);
                            evt.value_hi = cb->trip_count;
                            rb->try_push(evt);
                        }
                        // Fall through — admit the probe message.
                    } else {
                        return mailbox::EnqueueResult{
                            mailbox::EnqueueResultCode::CircuitOpen, target};
                    }
                } else if (cb->state == CircuitBreakerState::kHalfOpen) {
                    if (cb->half_open_probe_in_flight) {
                        return mailbox::EnqueueResult{
                            mailbox::EnqueueResultCode::CircuitOpen, target};
                    }
                    cb->half_open_probe_in_flight = true;
                }
            }
        }
    }
    // ── End circuit breaker admission gate ──────────────────────

    msg.set_deadline_ns(deadline_ns);

    if (auto dup =
            try_accept_duplicate(metrics_ring_buffer_.get(), dedup_cache_.get(),
                                 endpoint_, target, msg, options)) {
        return *dup;
    }
    if (auto expired = try_reject_expired(
            dead_letters_.get(), metrics_ring_buffer_.get(), endpoint_, target,
            msg, options, priority, deadline_ns)) {
        return *expired;
    }

    mailbox::MailboxEnvelopeMeta meta;
    meta.sender = msg.sender_address();
    meta.type_tag = msg.type_id();
    meta.priority = priority;
    meta.deadline_ns = deadline_ns;
    meta.flags = options.flags;
    meta.message_id = options.message_id;
    if (options.no_drop) {
        meta.flags |= net::WireFrame::NoDrop;
    }

    const auto bp_mode = mailbox->config().backpressure_mode;
    // Extract payload and trace before the move — needed for DLQ on rejection.
    const StreamBuffer& msg_payload = msg.payload();
    TraceContext msg_trace;
    bool msg_has_trace = msg.has_trace_context();
    if (msg_has_trace) {
        msg_trace = msg.trace_context();
    }
    auto result = mailbox->try_push(std::move(msg), meta);

    if (!result.accepted()) {
        emit_rejection_observability(
            dead_letters_.get(), metrics_ring_buffer_.get(), endpoint_, target,
            msg_payload, msg_trace, msg_has_trace, meta, result, options,
            mailbox->config().overflow_policy);
    }

    maybe_emit_backpressure_signal(mailbox, result, meta,
                                   options.emit_backpressure, bp_mode);

    return result;
}

mailbox::DeliveryResult
ActorSystem::deliver_with_result(ActorId target, TypedMessage msg,
                                 uint8_t priority, int64_t deadline_ns,
                                 mailbox::DeliveryOptions options) {
    auto er =
        try_deliver_local(target, std::move(msg), priority, deadline_ns, options);
    return mailbox::DeliveryResult::from_enqueue(er, ActorAddress{}, {});
}

void ActorSystem::deliver_local(ActorId target, TypedMessage msg) {
    (void)try_deliver_local(target, std::move(msg));
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
    (void)try_deliver_local(target, std::move(msg), priority, deadline_ns, {});
}

void ActorSystem::deliver_remote(const net::WireFrame& frame) {
    if (static_cast<TypeTag>(frame.pb_frame.type_tag()) ==
        TypeTag::BackpressureSignalTag) {
        (void)handle_remote_backpressure_signal(frame);
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
    // Pack the completion into a StreamBuffer for mailbox delivery.
    // The binary layout is a flat memcpy of the OpCompletion struct —
    // it is local-only (same process, same address space), so no
    // endianness or portability concerns.
    StreamBuffer payload(sizeof(net::OpCompletion));
    std::memcpy(payload.data(), &completion, sizeof(net::OpCompletion));

    TypedMessage msg(TypeTag::IoCompletionTag, std::move(payload));
    deliver_local(completion.actor, std::move(msg));
}

net::Transport* ActorSystem::get_transport_for(const EndPoint& /*endpoint*/) {
    // TcpTransport already handles per-endpoint routing via its internal pools_
    // map — TcpTransport::send() calls get_or_create_pool(target.endpoint)
    // internally. Return the single transport_ for all remote endpoints.
    if (!config_.enable_network) {
        return nullptr;
    }
    return transport_.get();
}

result<ActorRef> ActorSystem::spawn_remote(const std::string& node_name,
                                           const std::string& actor_type,
                                           const StreamBuffer& args) {
    return spawn_remote_async(node_name, actor_type, args).get();
}

AsyncActor ActorSystem::spawn_remote_async(const std::string& node_name,
                                           const std::string& actor_type,
                                           const StreamBuffer& args) {
    AsyncActor handle(endpoint_, config_.spawn_timeout_ms);

    if (!config_.enable_network || !transport_) {
        SpawnResponse resp;
        resp.error_code = spawn_errors::node_unreachable;
        handle.set_response(resp);
        return handle;
    }

    auto remote_endpoint = endpoint_ops::parse_endpoint(node_name);

    // Serialize spawn request using protobuf
    ::hpactor::SpawnRequestMessage pb_req;
    pb_req.set_actor_type_name(actor_type);
    pb_req.set_args_type(static_cast<uint32_t>(TypeTag::User));
    pb_req.set_serialized_args(reinterpret_cast<const char*>(args.data()),
                               args.size());
    net::to_proto(pb_req.mutable_supervisor(), system_actor_.address());

    StreamBuffer request_bytes = proto_registry_.serialize(pb_req);
    uint64_t msg_id = generate_message_id().value();

    net::WireFrame frame;
    net::to_proto(frame.pb_frame.mutable_sender(), system_actor_.address());
    net::to_proto(
        frame.pb_frame.mutable_receiver(),
        ActorAddress{remote_endpoint, SystemActorType, SpawnReceiverId, 0});
    frame.pb_frame.set_message_id(msg_id);
    frame.pb_frame.set_flags(net::WireFrame::RpcRequest);
    frame.pb_frame.set_payload(reinterpret_cast<const char*>(request_bytes.data()),
                               request_bytes.size());

    auto pending = std::make_shared<AsyncActor>(std::move(handle));
    pending->set_message_id(msg_id);

    {
        std::lock_guard<std::mutex> lock(pending_spawns_mutex_);
        pending_spawns_.emplace(msg_id, pending);
    }

    transport_->send(net::from_proto(frame.pb_frame.receiver()), frame.encode());

    return std::move(*pending);
}

// -----------------------------------------------------------------------------
// spawn_configured — spawn a pre-constructed actor with ActorDef config
// -----------------------------------------------------------------------------
Actor ActorSystem::spawn_configured(std::shared_ptr<AbstractActor> actor,
                                    const config::ActorDef& def) {
    FAULT_INJECT("hpactor.actor.spawn.fail") {
        return {};
    }
    ActorId id(next_actor_id_.fetch_add(1));
    actor->set_address(ActorAddress(endpoint_, actor->type(), id, 0));
    actor->set_type_name(def.behavior);

    {
        std::lock_guard<std::mutex> lock(actors_mutex_);
        actors_.emplace(id, actor);
    }

    // Create mailbox with capacity from ActorDef, capture pointer while lock is
    // held
    mailbox::MPSCActorMailbox<TypedMessage>* mbox = nullptr;
    {
        std::lock_guard<std::mutex> lock(mailboxes_mutex_);
        auto [it, _] = mailboxes_.emplace(
            id, std::make_unique<mailbox::MPSCActorMailbox<TypedMessage>>(
                    id, scheduler_.get(), mailbox_config_for_actor_def(def)));
        mbox = it->second.get();
    }

    // Create actor context and set it on the actor
    auto* local = static_cast<LocalActor*>(actor.get());
    auto actor_ctx = std::make_unique<ActorContext>(Actor(actor), this);
    local->set_context(actor_ctx.get());
    {
        std::lock_guard<std::mutex> lock(actor_contexts_mutex_);
        actor_contexts_.emplace(id, std::move(actor_ctx));
    }

    // Set scheduler and mailbox on actor
    actor->set_scheduler(scheduler_.get());
    actor->set_mailbox(mbox);

    // Register with scheduler. Actor class policy is authoritative for
    // specialized actors such as DaemonActor and DenseComputingActor; TOML can
    // only upgrade otherwise-cooperative actors.
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

    // Configure quarantine & circuit breaker for this actor
    if (def.quarantine.enabled) {
        if (auto* eba = actor->is_event_based_actor()
                            ? static_cast<EventBasedActor*>(actor.get())
                            : nullptr) {
            eba->configure_quarantine(def.quarantine);
        }
    }

    // Activate the actor (DaemonActor starts its thread here, etc.)
    local->on_activate();

    return Actor(actor);
}

// -----------------------------------------------------------------------------
// load_topology — convenience entry point for TOML-based bootstrapping
// -----------------------------------------------------------------------------
result<void> ActorSystem::load_topology(const std::string& toml_path) {
    auto parse_result = config::TomlParser::parse(toml_path);
    if (!parse_result.has_value()) {
        return result<void>::make(parse_result.error());
    }

    auto& model = parse_result.value();

    // Apply system-level metrics config from topology
    if (model.system.metrics_enabled) {
        metrics_config_.enabled = model.system.metrics_enabled;
        metrics_config_.ring_buffer_capacity =
            model.system.metrics_ring_buffer_capacity;
        metrics_config_.metrics_path = model.system.metrics_path;
    }

    // Apply system-level logging config from topology
    logging_config_ = model.system.logging;

// Apply shared system fields from topology
// NOLINTBEGIN(cppcoreguidelines-macro-usage)
#define HPACTOR_SYSTEM_TOML_FIELD(name, type, toml, def)                       \
    config_.name = static_cast<decltype(config_.name)>(model.system.name);
#include <hpactor/config/system_toml_fields.def>
#undef HPACTOR_SYSTEM_TOML_FIELD

// Apply mailbox defaults from topology
#define HPACTOR_MAILBOX_FIELD(name, type, toml, def)                           \
    config_.mailbox.name = model.system.mailbox.name;
#include <hpactor/config/mailbox_fields.def>
#undef HPACTOR_MAILBOX_FIELD
    // NOLINTEND(cppcoreguidelines-macro-usage)

    config_.dead_letters = model.system.dead_letters;
    dead_letters_ =
        std::make_unique<mailbox::DeadLetterQueue>(config_.dead_letters);

    apply_tracing_config(model.system.tracing);

    // Wire transport outbound limits and circuit breaker config into pool
    // config.
    config_.pool.outbound_limits = model.system.transport_outbound_limits;
    config_.pool.circuit_breaker_cfg = model.system.transport_circuit_breaker;
    if (transport_) {
        transport_->set_pool_config(config_.pool);
    }

    HPACTOR_LOG_INFO(log::LogCategory::kConfig, ActorId{0}, 0,
                     "topology bootstrap complete");

    // Validate all behaviors are registered
    auto& registry = config::ActorFactoryRegistry::instance();
    for (const auto& actor_def : model.actors) {
        if (!registry.has(actor_def.behavior)) {
            error err(errors::unknown);
            return result<void>::make(std::move(err));
        }
    }

    // Spawn actors in topological order; track numeric IDs for SystemInit
    std::vector<ActorId, mem::MemStdAllocator<ActorId>> spawned_ids(
        mem::MemStdAllocator<ActorId>(system_actor_.id(),
                                      mem::RegionType::kInternal));
    for (const auto& actor_def : model.actors) {
        auto factory = registry.get_factory(actor_def.behavior);
        auto actor_ptr = factory(nullptr, *this);

        Actor actor_handle = spawn_configured(std::move(actor_ptr), actor_def);

        // Register in name registry
        registry_.put(actor_def.id, actor_handle.address());

        spawned_ids.push_back(actor_handle.id());
    }

    // Broadcast SystemInit to all spawned actors
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

} // namespace hpactor

// ═══════════════════════════════════════════════════════════════════════════════
// Shutdown helpers (anonymous namespace, uses public ActorSystem API only)
// ═══════════════════════════════════════════════════════════════════════════════

namespace hpactor {
namespace {

struct ActorDrainInfo {
    ActorId id;
    bool is_system;
};

void initiate_actor_drain(ActorSystem& sys, ActorId id) {
    auto actor = sys.get_actor(id);
    if (!actor)
        return;

    auto* lc = actor->as_lifecycle();
    if (lc == nullptr) {
        // No lifecycle: call on_exit directly if EventBasedActor
        if (actor->is_event_based_actor()) {
            static_cast<EventBasedActor*>(actor.get())->on_exit();
        }
        return;
    }

    auto state = lc->state();
    // Skip actors that are already stopping or stopped
    if (state == LifecycleState::kStopping || state == LifecycleState::kStopped)
        return;

    auto policy = lc->drain_config().policy;

    if (policy == DrainPolicy::ImmediateStop) {
        // Drain mailbox synchronously (dead-letter all messages)
        if (actor->is_event_based_actor()) {
            static_cast<EventBasedActor*>(actor.get())->drain_all_immediate();
        } else {
            auto* mailbox = sys.get_mailbox(id);
            if (mailbox) {
                TypedMessage msg;
                while (mailbox->try_pop(msg)) {
                    // Messages dropped — equivalent to dead-lettering
                }
            }
        }
        // Drive lifecycle: kActive -> kStopping -> kStopped
        lc->transition(LifecycleState::kStopping);
        lc->transition(LifecycleState::kStopped);
        // Notify linked/monitored actors
        if (actor->is_event_based_actor()) {
            static_cast<EventBasedActor*>(actor.get())->on_exit();
        }
    } else {
        // Drain / DropUserMessages / deferred policies:
        // transition to kDraining, let EventBasedActor::receive() / drain
        // timer handle completion.
        if (state == LifecycleState::kActive) {
            lc->transition(LifecycleState::kDraining);
        }
        // Start drain timer if EventBasedActor
        if (actor->is_event_based_actor()) {
            static_cast<EventBasedActor*>(actor.get())->start_drain_timer();
        } else {
            // Non-EventBasedActor with lifecycle but no drain timer:
            // transition directly to stopped.
            lc->transition(LifecycleState::kStopping);
            lc->transition(LifecycleState::kStopped);
        }
    }
}

void poll_drain_complete(ActorSystem& sys, ActorId id,
                         std::chrono::steady_clock::time_point deadline) {
    while (std::chrono::steady_clock::now() < deadline) {
        auto actor = sys.get_actor(id);
        if (!actor)
            return; // Actor removed from registry

        auto* lc = actor->as_lifecycle();
        if (lc == nullptr)
            return; // No lifecycle — already handled

        if (lc->state() == LifecycleState::kStopped)
            return; // Drain complete

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

} // anonymous namespace
} // namespace hpactor

// ═══════════════════════════════════════════════════════════════════════════════
// ActorSystem — shutdown implementation
// ═══════════════════════════════════════════════════════════════════════════════

namespace hpactor {

result<void> ActorSystem::shutdown() {
    return shutdown(ShutdownOptions{});
}

result<void> ActorSystem::shutdown(const ShutdownOptions& opts) {
    ShutdownPhase phase = ShutdownPhase::Running;

    // Helper: check if we should force-stop (modifies phase/running in place)
    auto check_force = [&](std::chrono::steady_clock::time_point deadline) -> bool {
        if (!opts.force_after_timeout)
            return false;
        if (std::chrono::steady_clock::now() < deadline)
            return false;
        phase = ShutdownPhase::ForcedStop;
        shutdown_phase_.store(ShutdownPhase::ForcedStop, std::memory_order_release);
        running_.store(false, std::memory_order_release);
        return true;
    };

    // ── Phase: DrainingIngress ──────────────────────────────────────────
    phase = ShutdownPhase::DrainingIngress;
    shutdown_phase_.store(ShutdownPhase::DrainingIngress, std::memory_order_release);
    is_ready_.store(false, std::memory_order_release);

    auto ingress_deadline = std::chrono::steady_clock::now() + opts.ingress_timeout;
    // (HTTP gateway / remote spawn gating deferred to follow-up tasks)
    if (check_force(ingress_deadline))
        return result<void>::make();

    // ── Phase: DrainingActors ──────────────────────────────────────────
    phase = ShutdownPhase::DrainingActors;
    shutdown_phase_.store(ShutdownPhase::DrainingActors, std::memory_order_release);
    auto actor_deadline =
        std::chrono::steady_clock::now() + opts.actor_drain_timeout;

    // Collect actor IDs under lock, then drain in order
    {
        std::vector<ActorDrainInfo> actors;
        for_each_actor([&](ActorId id, AbstractActor& actor) {
            actors.push_back({id, actor.is_system_actor()});
        });
        // Lock released — safe to call into actors

        // Pass 1: initiate drain for non-system actors
        for (auto& info : actors) {
            if (info.is_system)
                continue;
            initiate_actor_drain(*this, info.id);
            if (check_force(actor_deadline))
                break;
        }
        // Poll non-system actors to completion
        if (!check_force(actor_deadline)) {
            for (const auto& info : actors) {
                if (info.is_system)
                    continue;
                poll_drain_complete(*this, info.id, actor_deadline);
                if (check_force(actor_deadline))
                    break;
            }
        }

        // Pass 2: initiate drain for system actors (last)
        if (!check_force(actor_deadline)) {
            for (auto& info : actors) {
                if (!info.is_system)
                    continue;
                initiate_actor_drain(*this, info.id);
                if (check_force(actor_deadline))
                    break;
            }
        }
        // Poll system actors to completion
        if (!check_force(actor_deadline)) {
            for (const auto& info : actors) {
                if (!info.is_system)
                    continue;
                poll_drain_complete(*this, info.id, actor_deadline);
                if (check_force(actor_deadline))
                    break;
            }
        }
    }

    if (check_force(actor_deadline))
        return result<void>::make();

    // ── Phase: LeavingCluster ──────────────────────────────────────────
    phase = ShutdownPhase::LeavingCluster;
    shutdown_phase_.store(ShutdownPhase::LeavingCluster, std::memory_order_release);
    auto leave_deadline =
        std::chrono::steady_clock::now() + opts.cluster_leave_timeout;
    // (Full implementation deferred until sharding)
    if (check_force(leave_deadline))
        return result<void>::make();

    // ── Phase: FlushingTelemetry ──────────────────────────────────────
    phase = ShutdownPhase::FlushingTelemetry;
    shutdown_phase_.store(ShutdownPhase::FlushingTelemetry,
                          std::memory_order_release);
    // Best-effort flush of logs, metrics, DLQ — no blocking

    // ── Phase: Stopped ─────────────────────────────────────────────────
    phase = ShutdownPhase::Stopped;
    shutdown_phase_.store(ShutdownPhase::Stopped, std::memory_order_release);
    running_.store(false, std::memory_order_release);
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
