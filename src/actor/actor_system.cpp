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

#include <hpactor/actor/spawn_receiver.hpp>
#include <hpactor/actor_type_registry.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/core/actor_system_ids.hpp>
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
actor_registry::actor_registry(CommunicationEndpoint endpoint)
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
          *this, config.scheduler_threads)),
      actor_type_registry_(std::make_unique<ActorTypeRegistry>()) {
    // Register system protobuf types
    proto_registry_.register_system_types();

    scheduler_->start();

    if (config.enable_network) {
        network_loop_ = std::make_unique<net::EventLoop>();
        network_loop_->set_actor_system(this);

        if (config.udp_port > 0) {
            registrar_ =
                std::make_unique<net::UdpRegistrar>(config.registrar, endpoint_);
            registrar_->start();
        }

        transport_ = std::make_unique<net::TcpTransport>(endpoint_, config.tls,
                                                         config.pool, nullptr);

        rpc_channel_ =
            std::make_unique<RpcChannel>(transport_.get(), scheduler_.get());

        http_client_ = std::make_unique<net::HttpClient>(network_loop_.get());

        transport_->set_rpc_handler(
            [this](hpactor::MessageId id, const hpactor::bytes& data) {
                rpc_channel_->on_response(id, data);
            });

        transport_->set_actor_message_handler(
            [this](const net::WireFrame& frame) {
                this->deliver_remote(frame);
            });

        if (config.tcp_port > 0) {
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

void ActorSystem::deliver_local(ActorId target, TypedMessage msg) {
    deliver_local(target, std::move(msg), 0, INT64_MAX);
}

void ActorSystem::deliver_local(ActorId target, TypedMessage msg,
                                uint8_t /*priority*/, int64_t /*deadline_ns*/) {
    auto* mailbox = get_mailbox(target);
    if (!mailbox)
        return;
    mailbox->push(std::move(msg));
}

void ActorSystem::deliver_remote(const net::WireFrame& frame) {
    TypedMessage msg(static_cast<TypeTag>(frame.type_tag), frame.payload);
    msg.set_sender_address(frame.sender);
    deliver_local(frame.receiver.id, std::move(msg));
}

void ActorSystem::enqueue_completion(net::OpCompletion completion) {
    // Convert I/O completion to a TypedMessage using a system tag placeholder.
    // TODO: define completion_msg in protobuf or use a dedicated internal path.
    (void)completion;
}

net::Transport*
ActorSystem::get_transport_for(const CommunicationEndpoint& /*endpoint*/) {
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
                          const std::string& actor_type, const bytes& /*args*/) {
    return spawn_remote_async(node_name, actor_type, bytes{}).get();
}

AsyncActor ActorSystem::spawn_remote_async(const std::string& node_name,
                                           const std::string& actor_type,
                                           const bytes& /*args*/) {
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
    // supervisor as ActorRef
    auto* pb_sup = pb_req.mutable_supervisor();
    auto sup_addr = system_actor_.address();
    // Set supervisor endpoint
    if (auto* ipv4 = std::get_if<Ipv4Endpoint>(&sup_addr.endpoint)) {
        pb_sup->mutable_endpoint()->mutable_ipv4()->set_addr(ipv4->addr);
        pb_sup->mutable_endpoint()->mutable_ipv4()->set_port(ipv4->port_nw);
    }
    pb_sup->set_type(sup_addr.type);
    pb_sup->set_actor_id(sup_addr.id.value());
    pb_sup->set_incarnation(sup_addr.incarnation);

    bytes request_bytes = proto_registry_.serialize(pb_req);

    net::WireFrame frame;
    frame.sender = system_actor_.address();
    frame.receiver =
        ActorAddress{remote_endpoint, SystemActorType, SpawnReceiverId, 0};
    frame.message_id = MessageId::generate().value();
    frame.flags = net::WireFrame::RpcRequest;
    frame.payload = request_bytes;

    auto pending = std::make_shared<AsyncActor>(std::move(handle));
    pending->set_message_id(frame.message_id);

    {
        std::lock_guard<std::mutex> lock(pending_spawns_mutex_);
        pending_spawns_.emplace(frame.message_id, pending);
    }

    transport_->send(frame.receiver, frame.encode());

    return std::move(*pending);
}

} // namespace hpactor
