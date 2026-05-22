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
#include <hpactor/log/logger.hpp>
#include <hpactor/net/frame.hpp>
#include <hpactor/spawn.hpp>

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
        // Log spawn failure with canonical failure reason
        auto reason = failure_reason(response.error_code);
        HPACTOR_LOG_WARNING(log::LogCategory::kActor, ActorId{0}, 0, "spawn_failure",
                            log::field_lit("type", req.actor_type_name.c_str()),
                            log::field_lit("reason", to_string(reason)),
                            log::field("retryable", retryable(reason)));
    }

    if (transport_ != nullptr) {
        // Serialize response using protobuf
        ::hpactor::SpawnResponseMessage pb_resp;
        net::to_proto(pb_resp.mutable_actor_addr(), response.actor_addr);
        pb_resp.set_error_code(response.error_code);

        net::WireFrame response_frame;
        net::to_proto(response_frame.pb_frame.mutable_sender(), address());
        response_frame.pb_frame.mutable_receiver()->CopyFrom(
            frame.pb_frame.sender());
        response_frame.pb_frame.set_message_id(frame.pb_frame.message_id());
        response_frame.pb_frame.set_flags(net::WireFrame::RpcResponse);
        response_frame.pb_frame.set_type_tag(
            static_cast<uint32_t>(TypeTag::SpawnResponseTag));
        auto serialized = system().proto_registry().serialize(pb_resp);
        response_frame.pb_frame.set_payload(
            reinterpret_cast<const char*>(serialized.data()), serialized.size());

        transport_->send(net::from_proto(response_frame.pb_frame.receiver()),
                         response_frame.encode());
    }
}

} // namespace hpactor
