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

#include <hpactor/core/actor_system.hpp>
#include <hpactor/sched/scheduler.hpp>
#include <hpactor/actor_type_registry.hpp>
#include <hpactor/spawn.hpp>
#include <hpactor/types/serialization.hpp>
#include <hpactor/net/frame.hpp>
#include <hpactor/net/tcp_transport.hpp>
#include <hpactor/actor/spawn_receiver.hpp>
#include <hpactor/core/actor_system_ids.hpp>

namespace hpactor {

// -----------------------------------------------------------------------------
// actor_registry implementation
// -----------------------------------------------------------------------------
actor_registry::actor_registry(NodeId node_id) : node_id_(node_id) {}

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
    : config_(config),
      node_id_(config.node_id),
      registry_(node_id_),
      scheduler_(std::make_unique<sched::HybridScheduler>(*this, config.scheduler_threads)),
      actor_type_registry_(std::make_unique<ActorTypeRegistry>()) {

    scheduler_->start();

    if (config.enable_network) {
        // Initialize network components
        network_loop_ = std::make_unique<net::EventLoop>();
        network_loop_->set_actor_system(this);

        // Create registrar first (before transport)
        if (config.udp_port > 0) {
            registrar_ = std::make_unique<net::UdpRegistrar>(
                config.registrar, node_id_);
            registrar_->start();
        }

        // Create transport with registry
        // Note: TcpTransport expects NodeRegistry*, but UdpRegistrar owns one internally.
        // For now, pass nullptr and rely on UdpRegistrar for discovery.
        transport_ = std::make_unique<net::TcpTransport>(
            node_id_, config.tls, config.pool, nullptr);

        // Initialize RPC channel after transport is created
        rpc_channel_ = std::make_unique<RpcChannel>(transport_.get(), scheduler_.get());

        // Set RPC response handler on transport to route responses to RPC channel
        transport_->set_rpc_handler(
            [this](hpactor::MessageId id, const hpactor::bytes& data) {
                rpc_channel_->on_response(id, data);
            });

        // Listen on TCP port if specified
        if (config.tcp_port > 0) {
            transport_->listen(config.tcp_port);
        }

        // Start network event loop in background thread
        network_thread_ = std::thread([this]() {
            while (network_loop_->wait(100) >= 0) {
                network_loop_->process_completions();
                if (!is_running()) break;
            }
        });

        // Spawn the SpawnReceiver system actor using well-known ID
        // SpawnReceiverId = ActorId(0xFFFF0001) is reserved for spawn handling
        auto spawn_receiver = std::make_shared<SpawnReceiver>(
            *this, *actor_type_registry_, transport_.get());
        spawn_receiver->set_address(
            ActorAddress{node_id_, SystemActorType, SpawnReceiverId, 0});

        {
            std::lock_guard<std::mutex> lock(actors_mutex_);
            actors_.emplace(SpawnReceiverId, spawn_receiver);
        }

        // Create mailbox for spawn receiver
        {
            std::lock_guard<std::mutex> lock(mailboxes_mutex_);
            mailboxes_.emplace(SpawnReceiverId,
                std::make_unique<mailbox::MPSCActorMailbox<Message<MessageVariant>>>(
                    SpawnReceiverId, scheduler_.get()));
        }
    }
}

ActorSystem::~ActorSystem() {
    if (config_.enable_network) {
        if (network_thread_.joinable()) {
            network_thread_.join();
        }
        if (transport_) {
            transport_->stop_listening();
        }
        if (registrar_) {
            registrar_->stop();
        }
    }
    scheduler_->stop();
}

void ActorSystem::register_actor(const std::string& name, Actor actor) {
    registry_.put(name, actor.address());
}

Actor ActorSystem::resolve_actor(const std::string& name) {
    ActorAddress addr = registry_.get(name);
    if (!addr) {
        return Actor{};
    }
    // Return an actor handle - actual resolution would require more
    // infrastructure
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

mailbox::MPSCActorMailbox<Message<MessageVariant>>* ActorSystem::get_mailbox(ActorId id) {
    std::lock_guard<std::mutex> lock(mailboxes_mutex_);
    auto it = mailboxes_.find(id);
    if (it != mailboxes_.end()) {
        return it->second.get();
    }
    return nullptr;
}

void ActorSystem::deliver_local(ActorId target, MessageVariant msg) {
    deliver_local(target, std::move(msg), 0, INT64_MAX);
}

void ActorSystem::deliver_local(ActorId target, MessageVariant msg,
                                uint8_t /*priority*/, int64_t /*deadline_ns*/) {
    auto* mailbox = get_mailbox(target);
    if (!mailbox) return;
    mailbox->push(Message<MessageVariant>(std::move(msg)));
}

void ActorSystem::enqueue_completion(net::OpCompletion completion) {
    completion_msg msg;
    msg.actor = completion.actor;
    msg.type = completion.type;
    msg.fd = completion.fd;
    msg.result = completion.result;
    msg.user_data = completion.user_data;

    deliver_local(completion.actor, std::move(msg));
}

result<ActorRef> ActorSystem::spawn_remote(const std::string& node_name,
                                           const std::string& actor_type,
                                           const bytes& /*args*/) {
    return spawn_remote_async(node_name, actor_type, bytes{}).get();
}

AsyncActor ActorSystem::spawn_remote_async(const std::string& node_name,
                                           const std::string& actor_type,
                                           const bytes& /*args*/) {
    AsyncActor handle(node_id_, config_.spawn_timeout);

    if (!config_.enable_network || !transport_) {
        // No network - mark as failed
        SpawnResponse resp;
        resp.error_code = spawn_errors::node_unreachable;
        handle.set_response(resp);
        return handle;
    }

    // TODO: Look up node by name via registrar
    // For now, assume node_name is a NodeId string
    NodeId remote_node_id = static_cast<NodeId>(std::stoul(node_name));

    // Serialize request using DefaultSerializer::encode_spawn
    DefaultSerializer serializer;
    SpawnRequest req;
    req.actor_type_name = actor_type;
    req.args_type = TypeTag::User;
    req.serialized_args = bytes{};
    req.supervisor_addr = system_actor_.address();  // Supervisor is system actor for now

    SpawnMessageVariant mv = req;
    bytes request_bytes = serializer.encode_spawn(TypeTag::SpawnRequestTag, mv);

    // Create frame for spawn request
    net::Frame frame;
    frame.sender = system_actor_.address();
    frame.receiver = ActorAddress{remote_node_id, SystemActorType, SpawnReceiverId, 0};
    frame.message_id = MessageId::generate().value();
    frame.flags = net::Frame::RpcRequest;
    frame.payload = request_bytes;

    // Create pending actor for response routing
    auto pending = std::make_shared<AsyncActor>(std::move(handle));
    pending->set_message_id(frame.message_id);

    {
        std::lock_guard<std::mutex> lock(pending_spawns_mutex_);
        pending_spawns_.emplace(frame.message_id, pending);
    }

    // Send via transport
    transport_->send(frame.receiver, frame.encode());

    return std::move(*pending);
}

} // namespace hpactor
