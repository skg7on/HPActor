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

// include/hpactor/net/registrar_serialization.hpp
#pragma once

#include <hpactor/net/registrar.hpp>
#include <hpactor/registrar.pb.h>
#include <hpactor/types/types.hpp>

#include <string>
#include <vector>

namespace hpactor {
namespace net {

// -----------------------------------------------------------------------------
// TCP message serialization
// -----------------------------------------------------------------------------

// PbRegisterPayload - acceptors are at top level per spec
inline PbRegisterPayload to_proto_register(const NodeEndpoint& ep) {
    PbRegisterPayload msg;
    msg.mutable_endpoint_info()->set_endpoint(
        endpoint_ops::to_string(ep.identity.endpoint));
    msg.mutable_endpoint_info()->set_host(ep.identity.host);
    msg.mutable_endpoint_info()->set_tcp_port(ep.tcp_port);
    for (const auto& acc : ep.identity.acceptors) {
        auto* a = msg.add_acceptors();
        a->set_port(acc.port);
        a->set_handshake_version(acc.handshake_version);
        a->set_protocol_version(acc.protocol_version);
        a->set_tls_required(acc.tls_required);
    }
    return msg;
}

inline StreamBuffer serialize_register_payload(const NodeEndpoint& ep) {
    PbRegisterPayload msg = to_proto_register(ep);
    std::string serialized = msg.SerializeAsString();
    return StreamBuffer(serialized.begin(), serialized.end());
}

inline bool
parse_register_payload(const StreamBuffer& data, PbRegisterPayload& msg) {
    return msg.ParseFromArray(data.data(), static_cast<int>(data.size()));
}

// PbAcceptPayload
inline PbAcceptPayload to_proto_accept(uint8_t error_code) {
    PbAcceptPayload msg;
    msg.set_error_code(error_code);
    return msg;
}

inline StreamBuffer serialize_accept_payload(uint8_t error_code) {
    PbAcceptPayload msg = to_proto_accept(error_code);
    std::string serialized = msg.SerializeAsString();
    return StreamBuffer(serialized.begin(), serialized.end());
}

inline bool parse_accept_payload(const StreamBuffer& data, PbAcceptPayload& msg) {
    return msg.ParseFromArray(data.data(), static_cast<int>(data.size()));
}

// PbNodeJoinPayload
inline PbNodeJoinPayload to_proto_node_join(const NodeEndpoint& ep) {
    PbNodeJoinPayload msg;
    msg.mutable_endpoint_info()->set_endpoint(
        endpoint_ops::to_string(ep.identity.endpoint));
    msg.mutable_endpoint_info()->set_host(ep.identity.host);
    msg.mutable_endpoint_info()->set_tcp_port(ep.tcp_port);
    return msg;
}

inline StreamBuffer serialize_node_join_payload(const NodeEndpoint& ep) {
    PbNodeJoinPayload msg = to_proto_node_join(ep);
    std::string serialized = msg.SerializeAsString();
    return StreamBuffer(serialized.begin(), serialized.end());
}

inline bool
parse_node_join_payload(const StreamBuffer& data, PbNodeJoinPayload& msg) {
    return msg.ParseFromArray(data.data(), static_cast<int>(data.size()));
}

// PbNodeLeavePayload
inline PbNodeLeavePayload to_proto_node_leave(const EndPoint& ep) {
    PbNodeLeavePayload msg;
    msg.set_endpoint(endpoint_ops::to_string(ep));
    return msg;
}

inline StreamBuffer serialize_node_leave_payload(const EndPoint& ep) {
    PbNodeLeavePayload msg = to_proto_node_leave(ep);
    std::string serialized = msg.SerializeAsString();
    return StreamBuffer(serialized.begin(), serialized.end());
}

inline bool
parse_node_leave_payload(const StreamBuffer& data, PbNodeLeavePayload& msg) {
    return msg.ParseFromArray(data.data(), static_cast<int>(data.size()));
}

// PbErrorPayload
inline PbErrorPayload to_proto_error(uint8_t code, const std::string& msg) {
    PbErrorPayload pb;
    pb.set_error_code(code);
    pb.set_message(msg);
    return pb;
}

inline StreamBuffer serialize_error_payload(uint8_t code, const std::string& msg) {
    PbErrorPayload pb = to_proto_error(code, msg);
    std::string serialized = pb.SerializeAsString();
    return StreamBuffer(serialized.begin(), serialized.end());
}

// -----------------------------------------------------------------------------
// UDP message serialization
// -----------------------------------------------------------------------------

// PbResolveQueryPayload
inline PbResolveQueryPayload to_proto_resolve_query(const EndPoint& ep) {
    PbResolveQueryPayload msg;
    msg.set_target_endpoint(endpoint_ops::to_string(ep));
    return msg;
}

inline StreamBuffer serialize_resolve_query_payload(const EndPoint& ep) {
    PbResolveQueryPayload msg = to_proto_resolve_query(ep);
    std::string serialized = msg.SerializeAsString();
    return StreamBuffer(serialized.begin(), serialized.end());
}

inline bool
parse_resolve_query_payload(const StreamBuffer& data, PbResolveQueryPayload& msg) {
    return msg.ParseFromArray(data.data(), static_cast<int>(data.size()));
}

// PbResolveResponsePayload
inline PbResolveResponsePayload to_proto_resolve_response(const NodeEndpoint& ep) {
    PbResolveResponsePayload msg;
    msg.mutable_endpoint_info()->set_endpoint(
        endpoint_ops::to_string(ep.identity.endpoint));
    msg.mutable_endpoint_info()->set_host(ep.identity.host);
    msg.mutable_endpoint_info()->set_tcp_port(ep.tcp_port);
    return msg;
}

inline StreamBuffer serialize_resolve_response_payload(const NodeEndpoint& ep) {
    PbResolveResponsePayload msg = to_proto_resolve_response(ep);
    std::string serialized = msg.SerializeAsString();
    return StreamBuffer(serialized.begin(), serialized.end());
}

inline bool parse_resolve_response_payload(const StreamBuffer& data,
                                           PbResolveResponsePayload& msg) {
    return msg.ParseFromArray(data.data(), static_cast<int>(data.size()));
}

} // namespace net
} // namespace hpactor