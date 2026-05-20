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

#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/actor/http_gateway_actor.hpp>
#include <hpactor/actor/local_actor.hpp>
#include <hpactor/actor/spawn_receiver.hpp>
#include <hpactor/actor_type_registry.hpp>
#include <hpactor/config/actor_factory_registry.hpp>
#include <hpactor/config/toml_parser.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/hpactor_config.hpp>

#include <hpactor/cli/cli_actor.hpp>
#include <hpactor/core/actor_system_ids.hpp>
#include <hpactor/log/log_manager.hpp>
#include <hpactor/log/logger.hpp>
#include <hpactor/mem/std_allocator.hpp>
#include <hpactor/net/async_io_fwd.hpp>
#include <hpactor/net/frame.hpp>
#include <hpactor/net/tcp_transport.hpp>
#include <hpactor/sched/scheduler.hpp>
#include <hpactor/spawn.hpp>

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

        rpc_channel_ =
            std::make_unique<RpcChannel>(transport_.get(), scheduler_.get());

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

void ActorSystem::register_actor(const std::string& name, Actor actor) {
    registry_.put(name, actor.address());
}

Actor ActorSystem::resolve_actor(const std::string& name) {
    ActorAddress addr = registry_.get(name);
    if (!addr) {
        return Actor{};
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
    cfg.protected_system_messages = config_.mailbox.protected_system_messages;
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
    cfg.max_overflow_depth = def.mailbox.max_overflow_depth;
    return cfg;
}

// -----------------------------------------------------------------------------
// try_deliver_local — bounded admission boundary
// -----------------------------------------------------------------------------
mailbox::EnqueueResult
ActorSystem::try_deliver_local(ActorId target, TypedMessage msg,
                               uint8_t priority, int64_t deadline_ns,
                               mailbox::DeliveryOptions options) {
    auto* mailbox = get_mailbox(target);
    if (mailbox == nullptr) {
        // Capture dead letter for missing actor
        if (dead_letters_) {
            mailbox::DeadLetterRecord dl;
            dl.reason = mailbox::DeadLetterReason::ActorNotFound;
            dl.source = mailbox::DeadLetterSource::LocalDelivery;
            dl.sender = msg.sender_address();
            dl.target = ActorAddress{endpoint_, ActorType{0}, target, 0};
            dl.type_tag = msg.type_id();
            dl.message_id = options.message_id;
            dl.frame_flags = options.flags;
            dl.priority = priority;
            dl.deadline_ns = deadline_ns;
            dl.payload_sample = msg.payload();
            (void)dead_letter(std::move(dl));
        }

        mailbox::EnqueueResult r;
        r.code = mailbox::EnqueueResultCode::ActorNotFound;
        r.target = target;
        return r;
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

    auto result = mailbox->try_push(std::move(msg), meta);

    // Capture dead letter when mailbox rejects and policy is DeadLetter
    if (!result.accepted() && mailbox->config().overflow_policy ==
                                  mailbox::OverflowPolicy::DeadLetter) {
        if (dead_letters_) {
            mailbox::DeadLetterRecord dl;
            dl.reason = mailbox::DeadLetterReason::OverflowPolicy;
            dl.source = mailbox::DeadLetterSource::MailboxAdmission;
            dl.sender = meta.sender;
            dl.target = ActorAddress{endpoint_, ActorType{0}, target, 0};
            dl.type_tag = meta.type_tag;
            dl.message_id = meta.message_id;
            dl.frame_flags = meta.flags;
            dl.priority = meta.priority;
            dl.deadline_ns = meta.deadline_ns;
            dl.mailbox_depth = result.depth;
            dl.mailbox_capacity = result.capacity;
            (void)dead_letter(std::move(dl));
        }
    }

    // Emit backpressure signal when target mailbox is under soft pressure
    if (result.code == mailbox::EnqueueResultCode::AcceptedWithSoftPressure &&
        options.emit_backpressure) {
        mailbox::BackpressureSignal signal;
        signal.target = ActorAddress{endpoint_, ActorType{0}, target, 0};
        signal.sender = meta.sender;
        signal.reason = mailbox::BackpressureReason::HighWatermark;
        signal.depth = result.depth;
        signal.capacity = result.capacity;
        signal.pressure_ratio = result.pressure_ratio;
        signal.retry_after = result.retry_after;
        signal_backpressure(signal);
    }

    return result;
}

void ActorSystem::deliver_local(ActorId target, TypedMessage msg) {
    (void)try_deliver_local(target, std::move(msg));
}

void ActorSystem::deliver_local(ActorId target, TypedMessage msg,
                                uint8_t priority, int64_t deadline_ns) {
    (void)try_deliver_local(target, std::move(msg), priority, deadline_ns, {});
}

void ActorSystem::deliver_remote(const net::WireFrame& frame) {
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
                                           const StreamBuffer& /*args*/) {
    return spawn_remote_async(node_name, actor_type, StreamBuffer{}).get();
}

AsyncActor ActorSystem::spawn_remote_async(const std::string& node_name,
                                           const std::string& actor_type,
                                           const StreamBuffer& /*args*/) {
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
    ActorId id(next_actor_id_.fetch_add(1));
    actor->set_address(ActorAddress(endpoint_, actor->type(), id, 0));
    actor->set_type_name(def.behavior);

    {
        std::lock_guard<std::mutex> lock(actors_mutex_);
        actors_.emplace(id, actor);
    }

    // Create mailbox with capacity from ActorDef
    {
        std::lock_guard<std::mutex> lock(mailboxes_mutex_);
        mailboxes_.emplace(
            id, std::make_unique<mailbox::MPSCActorMailbox<TypedMessage>>(
                    id, scheduler_.get(), mailbox_config_for_actor_def(def)));
    }

    // Create actor context and set it on the actor
    auto* local = static_cast<LocalActor*>(actor.get());
    auto actor_ctx = std::make_unique<ActorContext>(Actor(actor), this);
    local->set_context(actor_ctx.get());
    actor_contexts_.emplace(id, std::move(actor_ctx));

    // Set scheduler and mailbox on actor
    actor->set_scheduler(scheduler_.get());
    actor->set_mailbox(mailboxes_[id].get());

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
