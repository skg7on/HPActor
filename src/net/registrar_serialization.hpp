// src/net/registrar_serialization.hpp
#pragma once

#include <hpactor/net/registrar.hpp>
#include <hpactor/types/types.hpp>
#include <hpactor/net/registrar.pb.h>

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
    msg.mutable_endpoint_info()->set_endpoint(endpoint_ops::to_string(ep.endpoint));
    msg.mutable_endpoint_info()->set_host(ep.host);
    msg.mutable_endpoint_info()->set_tcp_port(ep.tcp_port);
    for (const auto& acc : ep.acceptors) {
        auto* a = msg.add_acceptors();
        a->set_port(acc.port);
        a->set_handshake_version(acc.handshake_version);
        a->set_protocol_version(acc.protocol_version);
        a->set_tls_required(acc.tls_required);
    }
    return msg;
}

inline bytes serialize_register_payload(const NodeEndpoint& ep) {
    PbRegisterPayload msg = to_proto_register(ep);
    return bytes(msg.SerializeAsString().begin(), msg.SerializeAsString().end());
}

inline PbRegisterPayload parse_register_payload(const bytes& data) {
    PbRegisterPayload msg;
    msg.ParseFromArray(data.data(), static_cast<int>(data.size()));
    return msg;
}

// PbAcceptPayload
inline PbAcceptPayload to_proto_accept(uint8_t error_code) {
    PbAcceptPayload msg;
    msg.set_error_code(error_code);
    return msg;
}

inline bytes serialize_accept_payload(uint8_t error_code) {
    PbAcceptPayload msg = to_proto_accept(error_code);
    return bytes(msg.SerializeAsString().begin(), msg.SerializeAsString().end());
}

inline PbAcceptPayload parse_accept_payload(const bytes& data) {
    PbAcceptPayload msg;
    msg.ParseFromArray(data.data(), static_cast<int>(data.size()));
    return msg;
}

// PbNodeJoinPayload
inline PbNodeJoinPayload to_proto_node_join(const NodeEndpoint& ep) {
    PbNodeJoinPayload msg;
    msg.mutable_endpoint_info()->set_endpoint(endpoint_ops::to_string(ep.endpoint));
    msg.mutable_endpoint_info()->set_host(ep.host);
    msg.mutable_endpoint_info()->set_tcp_port(ep.tcp_port);
    return msg;
}

inline bytes serialize_node_join_payload(const NodeEndpoint& ep) {
    PbNodeJoinPayload msg = to_proto_node_join(ep);
    return bytes(msg.SerializeAsString().begin(), msg.SerializeAsString().end());
}

inline PbNodeJoinPayload parse_node_join_payload(const bytes& data) {
    PbNodeJoinPayload msg;
    msg.ParseFromArray(data.data(), static_cast<int>(data.size()));
    return msg;
}

// PbNodeLeavePayload
inline PbNodeLeavePayload to_proto_node_leave(const CommunicationEndpoint& ep) {
    PbNodeLeavePayload msg;
    msg.set_endpoint(endpoint_ops::to_string(ep));
    return msg;
}

inline bytes serialize_node_leave_payload(const CommunicationEndpoint& ep) {
    PbNodeLeavePayload msg = to_proto_node_leave(ep);
    return bytes(msg.SerializeAsString().begin(), msg.SerializeAsString().end());
}

inline PbNodeLeavePayload parse_node_leave_payload(const bytes& data) {
    PbNodeLeavePayload msg;
    msg.ParseFromArray(data.data(), static_cast<int>(data.size()));
    return msg;
}

// PbErrorPayload
inline PbErrorPayload to_proto_error(uint8_t code, const std::string& msg) {
    PbErrorPayload pb;
    pb.set_error_code(code);
    pb.set_message(msg);
    return pb;
}

inline bytes serialize_error_payload(uint8_t code, const std::string& msg) {
    PbErrorPayload pb = to_proto_error(code, msg);
    return bytes(pb.SerializeAsString().begin(), pb.SerializeAsString().end());
}

// -----------------------------------------------------------------------------
// UDP message serialization
// -----------------------------------------------------------------------------

// PbResolveQueryPayload
inline PbResolveQueryPayload to_proto_resolve_query(const CommunicationEndpoint& ep) {
    PbResolveQueryPayload msg;
    msg.set_target_endpoint(endpoint_ops::to_string(ep));
    return msg;
}

inline bytes serialize_resolve_query_payload(const CommunicationEndpoint& ep) {
    PbResolveQueryPayload msg = to_proto_resolve_query(ep);
    return bytes(msg.SerializeAsString().begin(), msg.SerializeAsString().end());
}

inline PbResolveQueryPayload parse_resolve_query_payload(const bytes& data) {
    PbResolveQueryPayload msg;
    msg.ParseFromArray(data.data(), static_cast<int>(data.size()));
    return msg;
}

// PbResolveResponsePayload
inline PbResolveResponsePayload to_proto_resolve_response(const NodeEndpoint& ep) {
    PbResolveResponsePayload msg;
    msg.mutable_endpoint_info()->set_endpoint(endpoint_ops::to_string(ep.endpoint));
    msg.mutable_endpoint_info()->set_host(ep.host);
    msg.mutable_endpoint_info()->set_tcp_port(ep.tcp_port);
    return msg;
}

inline bytes serialize_resolve_response_payload(const NodeEndpoint& ep) {
    PbResolveResponsePayload msg = to_proto_resolve_response(ep);
    return bytes(msg.SerializeAsString().begin(), msg.SerializeAsString().end());
}

inline PbResolveResponsePayload parse_resolve_response_payload(const bytes& data) {
    PbResolveResponsePayload msg;
    msg.ParseFromArray(data.data(), static_cast<int>(data.size()));
    return msg;
}

} // namespace net
} // namespace hpactor
