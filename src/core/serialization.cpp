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

#include <hpactor/types/serialization.hpp>
#include <hpactor/spawn.hpp>
#include <hpactor/net/frame.hpp>

// Include the generated protobuf headers
#include <hpactor/common.pb.h>
#include <hpactor/messages.pb.h>

namespace hpactor {

namespace {

// -----------------------------------------------------------------------------
// Protobuf conversion helpers (same pattern as frame_protobuf.cpp)
// -----------------------------------------------------------------------------

// Helper: convert HPActor Ipv4Endpoint to protobuf PbIpv4Endpoint
static void to_proto(::hpactor::PbIpv4Endpoint* pb_ep, const ::hpactor::Ipv4Endpoint& ep) {
    pb_ep->set_addr(ep.addr);  // Network byte order
    pb_ep->set_port(ep.port_nw);
}

// Helper: convert HPActor Ipv6Endpoint to protobuf PbIpv6Endpoint
static void to_proto(::hpactor::PbIpv6Endpoint* pb_ep, const ::hpactor::Ipv6Endpoint& ep) {
    pb_ep->set_addr(ep.addr.data(), 16);
    pb_ep->set_port(ep.port_nw);
}

// Helper: convert HPActor CommunicationEndpoint to protobuf PbActorEndpoint
static void to_proto(::hpactor::PbActorEndpoint* pb_endpoint, const ::hpactor::CommunicationEndpoint& ep) {
    if (auto* ipv4 = std::get_if<::hpactor::Ipv4Endpoint>(&ep)) {
        to_proto(pb_endpoint->mutable_ipv4(), *ipv4);
    } else if (auto* ipv6 = std::get_if<::hpactor::Ipv6Endpoint>(&ep)) {
        to_proto(pb_endpoint->mutable_ipv6(), *ipv6);
    }
}

// Helper: convert protobuf PbIpv4Endpoint to HPActor Ipv4Endpoint
static ::hpactor::Ipv4Endpoint from_proto(const ::hpactor::PbIpv4Endpoint& pb_ep) {
    return ::hpactor::Ipv4Endpoint{pb_ep.addr(), static_cast<uint16_t>(pb_ep.port())};
}

// Helper: convert protobuf PbIpv6Endpoint to HPActor Ipv6Endpoint
static ::hpactor::Ipv6Endpoint from_proto(const ::hpactor::PbIpv6Endpoint& pb_ep) {
    std::array<uint8_t, 16> addr;
    std::memcpy(addr.data(), pb_ep.addr().data(), 16);
    return ::hpactor::Ipv6Endpoint{addr, static_cast<uint16_t>(pb_ep.port())};
}

// Helper: convert protobuf PbActorEndpoint to HPActor CommunicationEndpoint
static ::hpactor::CommunicationEndpoint from_proto(const ::hpactor::PbActorEndpoint& pb_endpoint) {
    if (pb_endpoint.has_ipv4()) {
        return from_proto(pb_endpoint.ipv4());
    } else if (pb_endpoint.has_ipv6()) {
        return from_proto(pb_endpoint.ipv6());
    }
    return ::hpactor::Ipv4Endpoint{};
}

// Helper: convert HPActor ActorAddress to protobuf PbActorAddress
static void to_proto(::hpactor::PbActorAddress* pb_addr, const ::hpactor::ActorAddress& addr) {
    to_proto(pb_addr->mutable_endpoint(), addr.endpoint);
    pb_addr->set_type(addr.type);
    pb_addr->set_actor_id(addr.id.value());
    pb_addr->set_incarnation(addr.incarnation);
}

// Helper: convert protobuf PbActorAddress to HPActor ActorAddress
static ::hpactor::ActorAddress from_proto(const ::hpactor::PbActorAddress& pb_addr) {
    return ::hpactor::ActorAddress{
        from_proto(pb_addr.endpoint()),
        static_cast<::hpactor::ActorType>(pb_addr.type()),
        ::hpactor::ActorId{pb_addr.actor_id()},
        pb_addr.incarnation()
    };
}

// Helper: convert HPActor ActorAddress to protobuf PbActorRef
static void to_proto(::hpactor::PbActorRef* pb_ref, const ::hpactor::ActorAddress& addr) {
    to_proto(pb_ref->mutable_endpoint(), addr.endpoint);
    pb_ref->set_type(addr.type);
    pb_ref->set_actor_id(addr.id.value());
    pb_ref->set_incarnation(addr.incarnation);
}

// Helper: convert protobuf PbActorRef to HPActor ActorAddress
static ::hpactor::ActorAddress from_proto(const ::hpactor::PbActorRef& pb_ref) {
    return ::hpactor::ActorAddress{
        from_proto(pb_ref.endpoint()),
        static_cast<::hpactor::ActorType>(pb_ref.type()),
        ::hpactor::ActorId{pb_ref.actor_id()},
        pb_ref.incarnation()
    };
}

} // anonymous namespace

// -----------------------------------------------------------------------------
// DefaultSerializer implementation
// -----------------------------------------------------------------------------

DefaultSerializer::DefaultSerializer() {
    // Register system message encoders/decoders
    // These are handled via std::visit in encode_system/decode_system
}

bytes DefaultSerializer::encode(TypeTag tag, const MessageVariant& msg) {
    // Check if it's a system message
    if (static_cast<uint32_t>(tag) < static_cast<uint32_t>(TypeTag::User)) {
        return encode_system(msg);
    }

    // User type - use registered encoder
    auto it = encoders_.find(tag);
    if (it != encoders_.end()) {
        bytes result;
        std::visit([&result, tag, this, &msg](const auto& m) {
            using T = std::decay_t<decltype(m)>;
            if constexpr (std::is_same_v<T, down_msg> ||
                        std::is_same_v<T, exit_msg> ||
                        std::is_same_v<T, link_msg> ||
                        std::is_same_v<T, unlink_msg>) {
                result = encode_system(msg);
            } else {
                // User type - use encoder if registered
                auto encoder_it = encoders_.find(tag);
                if (encoder_it != encoders_.end()) {
                    result = encoder_it->second(&m);
                }
            }
        }, msg);
        return result;
    }

    return bytes{};
}

MessageVariant DefaultSerializer::decode(TypeTag tag, const bytes& data) {
    // Check if it's a system message
    if (static_cast<uint32_t>(tag) < static_cast<uint32_t>(TypeTag::User)) {
        return decode_system(tag, data);
    }

    // User type - use registered decoder
    auto it = decoders_.find(tag);
    if (it != decoders_.end()) {
        // For now, return empty - actual decode requires knowing the type
        // This will be implemented when we have proper type erasure
    }

    return MessageVariant{};
}

void DefaultSerializer::register_type(TypeTag tag, encode_func encode, decode_func decode) {
    encoders_[tag] = std::move(encode);
    decoders_[tag] = std::move(decode);
}

bytes DefaultSerializer::encode_system(const MessageVariant& msg) {
    // down_msg
    if (std::holds_alternative<down_msg>(msg)) {
        const down_msg& m = std::get<down_msg>(msg);
        ::hpactor::DownMessage pb_msg;
        to_proto(pb_msg.mutable_endpoint(), m.terminated_actor.endpoint);
        pb_msg.set_actor_id(m.terminated_actor.id.value());
        pb_msg.set_reason_code(m.reason.code());
        std::string serialized = pb_msg.SerializeAsString();
        return bytes(serialized.begin(), serialized.end());
    }
    // exit_msg
    else if (std::holds_alternative<exit_msg>(msg)) {
        const exit_msg& m = std::get<exit_msg>(msg);
        ::hpactor::ExitMessage pb_msg;
        to_proto(pb_msg.mutable_sender(), m.sender.endpoint);
        pb_msg.set_actor_id(m.sender.id.value());
        pb_msg.set_reason_code(m.reason.code());
        std::string serialized = pb_msg.SerializeAsString();
        return bytes(serialized.begin(), serialized.end());
    }
    // link_msg
    else if (std::holds_alternative<link_msg>(msg)) {
        const link_msg& m = std::get<link_msg>(msg);
        ::hpactor::LinkMessage pb_msg;
        to_proto(pb_msg.mutable_target(), m.target.endpoint);
        pb_msg.set_actor_id(m.target.id.value());
        std::string serialized = pb_msg.SerializeAsString();
        return bytes(serialized.begin(), serialized.end());
    }
    // unlink_msg
    else if (std::holds_alternative<unlink_msg>(msg)) {
        const unlink_msg& m = std::get<unlink_msg>(msg);
        ::hpactor::UnlinkMessage pb_msg;
        to_proto(pb_msg.mutable_target(), m.target.endpoint);
        pb_msg.set_actor_id(m.target.id.value());
        std::string serialized = pb_msg.SerializeAsString();
        return bytes(serialized.begin(), serialized.end());
    }

    return bytes{};
}

MessageVariant DefaultSerializer::decode_system(TypeTag tag, const bytes& data) {
    std::string serialized(data.begin(), data.end());

    switch (tag) {
    case TypeTag::DownMsg: {
        ::hpactor::DownMessage pb_msg;
        if (!pb_msg.ParseFromString(serialized)) {
            return MessageVariant{};
        }
        down_msg m;
        m.terminated_actor.endpoint = from_proto(pb_msg.endpoint());
        m.terminated_actor.id = ActorId(pb_msg.actor_id());
        m.terminated_actor.type = 0;  // Type not stored in protobuf
        m.terminated_actor.incarnation = 0;  // Incarnation not stored in protobuf
        m.reason = error(pb_msg.reason_code());
        return m;
    }
    case TypeTag::ExitMsg: {
        ::hpactor::ExitMessage pb_msg;
        if (!pb_msg.ParseFromString(serialized)) {
            return MessageVariant{};
        }
        exit_msg m;
        m.sender.endpoint = from_proto(pb_msg.sender());
        m.sender.id = ActorId(pb_msg.actor_id());
        m.sender.type = 0;  // Type not stored in protobuf
        m.sender.incarnation = 0;  // Incarnation not stored in protobuf
        m.reason = error(pb_msg.reason_code());
        return m;
    }
    case TypeTag::LinkMsg: {
        ::hpactor::LinkMessage pb_msg;
        if (!pb_msg.ParseFromString(serialized)) {
            return MessageVariant{};
        }
        link_msg m;
        m.target.endpoint = from_proto(pb_msg.target());
        m.target.id = ActorId(pb_msg.actor_id());
        m.target.type = 0;  // Type not stored in protobuf
        m.target.incarnation = 0;  // Incarnation not stored in protobuf
        return m;
    }
    case TypeTag::UnlinkMsg: {
        ::hpactor::UnlinkMessage pb_msg;
        if (!pb_msg.ParseFromString(serialized)) {
            return MessageVariant{};
        }
        unlink_msg m;
        m.target.endpoint = from_proto(pb_msg.target());
        m.target.id = ActorId(pb_msg.actor_id());
        m.target.type = 0;  // Type not stored in protobuf
        m.target.incarnation = 0;  // Incarnation not stored in protobuf
        return m;
    }
    default:
        return MessageVariant{};
    }
}

bytes DefaultSerializer::encode_spawn([[maybe_unused]] TypeTag tag, const SpawnMessageVariant& msg) {
    // SpawnRequest
    if (std::holds_alternative<SpawnRequest>(msg)) {
        const SpawnRequest& m = std::get<SpawnRequest>(msg);
        ::hpactor::SpawnRequestMessage pb_msg;
        pb_msg.set_actor_type_name(m.actor_type_name);
        pb_msg.set_args_type(static_cast<uint32_t>(m.args_type));
        pb_msg.set_serialized_args(m.serialized_args.data(), m.serialized_args.size());
        to_proto(pb_msg.mutable_supervisor(), m.supervisor_addr);
        std::string serialized = pb_msg.SerializeAsString();
        return bytes(serialized.begin(), serialized.end());
    }
    // SpawnResponse
    else if (std::holds_alternative<SpawnResponse>(msg)) {
        const SpawnResponse& m = std::get<SpawnResponse>(msg);
        ::hpactor::SpawnResponseMessage pb_msg;
        to_proto(pb_msg.mutable_actor_addr(), m.actor_addr);
        pb_msg.set_error_code(m.error_code);
        std::string serialized = pb_msg.SerializeAsString();
        return bytes(serialized.begin(), serialized.end());
    }

    return bytes{};
}

SpawnMessageVariant DefaultSerializer::decode_spawn(TypeTag tag, const bytes& data) {
    std::string serialized(data.begin(), data.end());

    switch (tag) {
    case TypeTag::SpawnRequestTag: {
        ::hpactor::SpawnRequestMessage pb_msg;
        if (!pb_msg.ParseFromString(serialized)) {
            return SpawnMessageVariant{};
        }
        SpawnRequest m;
        m.actor_type_name = pb_msg.actor_type_name();
        m.args_type = static_cast<TypeTag>(pb_msg.args_type());
        m.serialized_args.assign(pb_msg.serialized_args().begin(), pb_msg.serialized_args().end());
        m.supervisor_addr = from_proto(pb_msg.supervisor());
        return m;
    }
    case TypeTag::SpawnResponseTag: {
        ::hpactor::SpawnResponseMessage pb_msg;
        if (!pb_msg.ParseFromString(serialized)) {
            return SpawnMessageVariant{};
        }
        SpawnResponse m;
        m.actor_addr = from_proto(pb_msg.actor_addr());
        m.error_code = pb_msg.error_code();
        return m;
    }
    default:
        return SpawnMessageVariant{};
    }
}

} // namespace hpactor
