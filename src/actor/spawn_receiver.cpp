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
#include <hpactor/core/actor_system.hpp>
#include <hpactor/net/frame.hpp>

#include <hpactor/messages.pb.h>

namespace hpactor {

SpawnReceiver::SpawnReceiver(ActorSystem& sys, ActorTypeRegistry& registry,
                             net::Transport* transport)
    : EventBasedActor(nullptr, sys), registry_(registry), transport_(transport) {}

Behavior SpawnReceiver::make_behavior() {
    return Behavior{[this](TypedMessage& msg) {
        if (msg.type_id() == TypeTag::SpawnRequestTag) {
            // Deserialize the protobuf spawn request
            auto pb_req = msg.as<::hpactor::SpawnRequestMessage>();
            if (pb_req) {
                // Convert protobuf to SpawnRequest
                SpawnRequest req;
                req.actor_type_name = pb_req->actor_type_name();
                req.args_type = static_cast<TypeTag>(pb_req->args_type());
                req.serialized_args.assign(pb_req->serialized_args().begin(),
                                           pb_req->serialized_args().end());
                handle_spawn_request(req, net::WireFrame{});
            }
        }
    }};
}

void SpawnReceiver::handle_spawn_request(const SpawnRequest& req,
                                         const net::WireFrame& frame) {
    SpawnResponse response;

    auto result = registry_.spawn(system(), req.actor_type_name,
                                  req.serialized_args, req.args_type);
    if (result.has_value()) {
        response.actor_addr = result.value();
        response.error_code = spawn_errors::success;
    } else {
        response.error_code = result.error().code();
    }

    if (transport_) {
        // Serialize response using protobuf
        ::hpactor::SpawnResponseMessage pb_resp;
        auto* pb_addr = pb_resp.mutable_actor_addr();
        auto& addr = response.actor_addr;
        if (auto* ipv4 = std::get_if<Ipv4Endpoint>(&addr.endpoint)) {
            pb_addr->mutable_endpoint()->mutable_ipv4()->set_addr(ipv4->addr);
            pb_addr->mutable_endpoint()->mutable_ipv4()->set_port(ipv4->port_nw);
        }
        pb_addr->set_type(addr.type);
        pb_addr->set_actor_id(addr.id.value());
        pb_addr->set_incarnation(addr.incarnation);
        pb_resp.set_error_code(response.error_code);

        net::WireFrame response_frame;
        response_frame.sender = address();
        response_frame.receiver = frame.sender;
        response_frame.message_id = frame.message_id;
        response_frame.flags = net::WireFrame::RpcResponse;
        response_frame.type_tag = static_cast<uint32_t>(TypeTag::SpawnResponseTag);
        response_frame.payload = system().proto_registry().serialize(pb_resp);

        transport_->send(response_frame.receiver, response_frame.encode());
    }
}

} // namespace hpactor
