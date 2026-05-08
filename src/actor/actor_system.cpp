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

#include <hpactor/actor/http_gateway_actor.hpp>
#include <hpactor/actor/local_actor.hpp>
#include <hpactor/actor/spawn_receiver.hpp>
#include <hpactor/actor_type_registry.hpp>
#include <hpactor/config/actor_factory_registry.hpp>
#include <hpactor/config/toml_parser.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/hpactor_config.hpp>

#if HPACTOR_ENABLE_CLI
#include <hpactor/cli/cli_actor.hpp>
#endif
#include <hpactor/mem/std_allocator.hpp>
#include <hpactor/core/actor_system_ids.hpp>
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
actor_registry::actor_registry(EndPoint endpoint)
    : endpoint_(endpoint) {}

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
          *this, config.scheduler_threads, 4, config.timer_backend)),
      actor_type_registry_(std::make_unique<ActorTypeRegistry>()) {
    // Register system protobuf types
    proto_registry_.register_system_types();

    scheduler_->start();

    // Initialize metrics subsystem (before actors so instrumentation is ready)
    if (metrics_config_.enabled) {
        metrics_ring_buffer_ =
            std::make_shared<metrics::MpscRingBuffer<metrics::MetricEvent>>();
        scheduler_->set_metrics_ring_buffer(metrics_ring_buffer_.get());
    }

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
            registrar_ = reg;  // shared ownership for registrar() accessor
        } else {
            discovery_ = std::make_shared<net::StaticDiscovery>(
                std::vector<net::Member>{});
        }

        discovery_->start();

        discovery_->on_member_change([this](const net::Member& m, bool joined) {
            if (!joined) {
                on_node_dead(m.endpoint);
            }
            // Note: proactive connection pool warming (prewarm_pool) will be
            // integrated in a follow-up task when ConnectionPool is updated.
        });

        location_cache_ = std::make_shared<net::ActorLocationCache>();
        if (network_loop_) {
            cache_purge_timer_ = network_loop_->run_every(
                [this]() {
                    if (location_cache_) location_cache_->purge_expired();
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
                [this](hpactor::MessageId id, const hpactor::StreamBuffer& data) {
                    rpc_channel_->on_response(id, data);
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
                    SpawnReceiverId, scheduler_.get()));
        }
    }

    // Spawn CLI actor
#if HPACTOR_ENABLE_CLI
    if (config_.cli.enabled) {
        auto spawned = spawn<cli::CliActor>(config_.cli);
        cli_actor_ = std::static_pointer_cast<cli::CliActor>(spawned.get());
    }
#endif
}

ActorSystem::~ActorSystem() {
    if (config_.enable_network) {
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
    scheduler_->stop();
}

void ActorSystem::on_node_dead(EndPoint dead_ep) {
    // Find all actors linked to or monitoring actors on the dead endpoint.
    // Uses the internal actor_contexts_ map directly (actor_context() is
    // a protected member of AbstractActor, not accessible from ActorSystem).
    std::lock_guard<std::mutex> lock(actor_contexts_mutex_);
    for (const auto& [id, ctx] : actor_contexts_) {
        if (!ctx) continue;
        for (const auto& addr : ctx->linked_actors()) {
            if (addr.endpoint == dead_ep) {
                TypedMessage down(TypeTag::DownMsg, StreamBuffer{});
                down.set_sender_address(ActorAddress{dead_ep, 0, ActorId(0), 0});
                deliver_local(id, std::move(down));
                break;
            }
        }
    }
    if (location_cache_) location_cache_->evict_node(dead_ep);
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

mailbox::MPSCActorMailbox<TypedMessage>*
ActorSystem::get_mailbox(ActorId id) {
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

#if HPACTOR_ENABLE_CLI
cli::CliActor* ActorSystem::cli_actor() const {
    return cli_actor_.get();
}
#endif

void ActorSystem::deliver_local(ActorId target, TypedMessage msg) {
    deliver_local(target, std::move(msg), 0, INT64_MAX);
}

void ActorSystem::deliver_local(ActorId target, TypedMessage msg,
                                uint8_t /*priority*/, int64_t /*deadline_ns*/) {
    auto* mailbox = get_mailbox(target);
    if (mailbox == nullptr) {
        return;
    }
    mailbox->push(std::move(msg));
}

void ActorSystem::deliver_remote(const net::WireFrame& frame) {
    StreamBuffer payload(frame.pb_frame.payload().begin(),
                         frame.pb_frame.payload().end());
    TypedMessage msg(static_cast<TypeTag>(frame.pb_frame.type_tag()),
                     std::move(payload));
    msg.set_sender_address(net::from_proto(frame.pb_frame.sender()));
    deliver_local(net::from_proto(frame.pb_frame.receiver()).id,
                  std::move(msg));
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

net::Transport*
ActorSystem::get_transport_for(const EndPoint& /*endpoint*/) {
    // TcpTransport already handles per-endpoint routing via its internal pools_
    // map — TcpTransport::send() calls get_or_create_pool(target.endpoint)
    // internally. Return the single transport_ for all remote endpoints.
    if (!config_.enable_network) {
        return nullptr;
    }
    return transport_.get();
}

result<ActorRef>
ActorSystem::spawn_remote(const std::string& node_name,
                          const std::string& actor_type, const StreamBuffer& /*args*/) {
    return spawn_remote_async(node_name, actor_type, StreamBuffer{}).get();
}

AsyncActor ActorSystem::spawn_remote_async(const std::string& node_name,
                                           const std::string& actor_type,
                                           const StreamBuffer& /*args*/) {
    AsyncActor handle(endpoint_, config_.spawn_timeout);

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
    uint64_t msg_id = MessageId::generate().value();

    net::WireFrame frame;
    net::to_proto(frame.pb_frame.mutable_sender(), system_actor_.address());
    net::to_proto(frame.pb_frame.mutable_receiver(),
                  ActorAddress{remote_endpoint, SystemActorType, SpawnReceiverId, 0});
    frame.pb_frame.set_message_id(msg_id);
    frame.pb_frame.set_flags(net::WireFrame::RpcRequest);
    frame.pb_frame.set_payload(
        reinterpret_cast<const char*>(request_bytes.data()),
        request_bytes.size());

    auto pending = std::make_shared<AsyncActor>(std::move(handle));
    pending->set_message_id(msg_id);

    {
        std::lock_guard<std::mutex> lock(pending_spawns_mutex_);
        pending_spawns_.emplace(msg_id, pending);
    }

    transport_->send(net::from_proto(frame.pb_frame.receiver()),
                     frame.encode());

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
                    id, scheduler_.get()));
    }

    // Create actor context and set it on the actor
    auto* local = static_cast<LocalActor*>(actor.get());
    auto actor_ctx = std::make_unique<ActorContext>(Actor(actor), this);
    local->set_context(actor_ctx.get());
    actor_contexts_.emplace(id, std::move(actor_ctx));

    // Set scheduler and mailbox on actor
    actor->set_scheduler(scheduler_.get());
    actor->set_mailbox(mailboxes_[id].get());

    // Register with scheduler based on ActorDef dispatch policy
    switch (def.dispatch_policy) {
    case config::DispatchPolicy::Cooperative:
        scheduler_->notify_ready(id, 0, INT64_MAX);
        break;
    case config::DispatchPolicy::DedicatedThread: {
        int cpu_aff = -1;
        scheduler_->register_dedicated_thread(id, cpu_aff);
        break;
    }
    case config::DispatchPolicy::DedicatedPool:
        scheduler_->register_dedicated_pool(id, 1);
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
        mem::MemStdAllocator<ActorId>(system_actor_.id(), mem::RegionType::kInternal));
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
