# Registrar Protobuf Serialization and Async UDP Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add protobuf serialization for registrar TCP/UDP messages and convert UDP I/O to async via EventLoop/AsyncIoBackend.

**Architecture:** Protobuf messages replace manual byte serialization in registrar protocol. UDP socket is registered with EventLoop and uses `async_recvfrom`/`async_sendto` for async I/O. `OpCompletion` struct is extended with source address fields for UDP recvfrom.

**Tech Stack:** C++20, protobuf, io_uring/GCD/kqueue async I/O

---

## File Structure

| File | Responsibility |
|------|----------------|
| `protos/hpactor/registrar.proto` | Protobuf definitions for all registrar messages |
| `protos/hpactor/CMakeLists.txt` | Add registrar.proto to protobuf build |
| `include/hpactor/net/async_io_backend.hpp` | Add src_addr/src_addr_len to OpCompletion |
| `src/net/registrar_serialization.hpp` | to_proto/from_proto declarations |
| `src/net/registrar_serialization.cpp` | to_proto/from_proto implementations |
| `src/net/registrar_server.cpp` | Use protobuf for message parsing |
| `src/net/registrar_client.cpp` | Use protobuf for message building |
| `src/net/registrar.cpp` | Add async UDP methods to UdpRegistrar |
| `tests/net/test_registrar_serialization.cpp` | Round-trip and malformed data tests |

---

## Task 1: Extend OpCompletion for UDP recvfrom

**Files:**
- Modify: `include/hpactor/net/async_io_backend.hpp:38-44`

- [ ] **Step 1: Modify OpCompletion struct**

```cpp
// In async_io_backend.hpp, replace OpCompletion struct (lines 38-44) with:

struct OpCompletion {
    ActorId actor;
    OpType  type;
    int     fd;         // fd the operation was on
    int     result;      // >= 0 bytes on success, < 0 errno on failure
    uint64_t user_data;  // original user_data from the SQE

    // For recvfrom: source address of received datagram
    struct sockaddr_in src_addr;
    socklen_t src_addr_len = 0;
};
```

- [ ] **Step 2: Verify build**

Run: `cmake -S . -B build -GNinja && ninja -C build hpactor_lib`
Expected: SUCCESS

- [ ] **Step 3: Commit**

```bash
git add include/hpactor/net/async_io_backend.hpp
git commit -m "feat(net): extend OpCompletion with src_addr for UDP recvfrom"
```

---

## Task 2: Create registrar.proto

**Files:**
- Create: `protos/hpactor/registrar.proto`
- Modify: `protos/hpactor/CMakeLists.txt` (or find where proto files are listed)

First, find where proto files are listed in CMakeLists.txt:

- [ ] **Step 1: Find proto file list in CMakeLists.txt**

Run: `grep -n "proto\|pb\|PROTOS" /Users/skg7on/Workspace/Projects/HPActor/CMakeLists.txt | head -30`

- [ ] **Step 2: Create registrar.proto**

```protobuf
// protos/hpactor/registrar.proto
syntax = "proto3";

package hpactor;

option optimize_for = SPEED;

// -----------------------------------------------------------------------------
// Common Types
// -----------------------------------------------------------------------------

message PbEndpointInfo {
    string endpoint = 1;  // Serialized CommunicationEndpoint (e.g., "127.0.0.1:5353")
    string host = 2;
    uint32 tcp_port = 3;
}

message PbAcceptorInfo {
    uint32 port = 1;
    uint32 handshake_version = 2;
    uint32 protocol_version = 3;
    bool tls_required = 4;
}

// -----------------------------------------------------------------------------
// TCP Messages (RegistrarConnection protocol)
// -----------------------------------------------------------------------------

// TCP Register message payload
message PbRegisterPayload {
    PbEndpointInfo endpoint_info = 1;
    repeated PbAcceptorInfo acceptors = 2;
}

// TCP Accept response payload
message PbAcceptPayload {
    uint32 error_code = 1;  // 0 = success
}

// TCP Heartbeat has no payload (empty message)
message PbHeartbeatPayload {
}

// TCP NodeJoin broadcast payload
message PbNodeJoinPayload {
    PbEndpointInfo endpoint_info = 1;
}

// TCP NodeLeave broadcast payload
message PbNodeLeavePayload {
    string endpoint = 1;  // Serialized CommunicationEndpoint
}

// TCP Error response payload
message PbErrorPayload {
    uint32 error_code = 1;
    string message = 2;
}

// -----------------------------------------------------------------------------
// UDP Messages (UdpRegistrar protocol)
// -----------------------------------------------------------------------------

// UDP ResolveQuery message payload
message PbResolveQueryPayload {
    string target_endpoint = 1;  // Serialized CommunicationEndpoint
}

// UDP ResolveResponse message payload
message PbResolveResponsePayload {
    PbEndpointInfo endpoint_info = 1;
}
```

**Note:** Message names match the spec exactly (`PbRegisterPayload` not `PbTcpRegisterPayload`, etc.) to ensure consistency.

- [ ] **Step 3: Add registrar.proto to build**

Find the list of proto files in CMakeLists.txt and add `protos/hpactor/registrar.proto` to the list alongside `common.proto`, `messages.proto`, `frame.proto`.

Run: `ninja -C build hpactor_proto` to generate C++ headers

Expected: SUCCESS (generates `registrar.pb.h` and `registrar.pb.cc`)

- [ ] **Step 4: Commit**

```bash
git add protos/hpactor/registrar.proto CMakeLists.txt
git commit -m "feat(proto): add registrar.proto for protobuf serialization"
```

---

## Task 3: Create registrar_serialization helpers

**Files:**
- Create: `src/net/registrar_serialization.hpp`
- Create: `src/net/registrar_serialization.cpp`

- [ ] **Step 1: Create registrar_serialization.hpp**

```cpp
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
```

- [ ] **Step 2: Create registrar_serialization.cpp**

```cpp
// src/net/registrar_serialization.cpp
// This file intentionally left empty - all helpers are inline in the header.
// This file exists to allow the build system to compile the translation unit
// and to provide a place for any non-inline serialization helpers if needed.
```

- [ ] **Step 3: Verify build**

Run: `ninja -C build hpactor_lib`
Expected: SUCCESS

- [ ] **Step 4: Commit**

```bash
git add src/net/registrar_serialization.hpp src/net/registrar_serialization.cpp
git commit -m "feat(net): add registrar protobuf serialization helpers"
```

---

## Task 4: Update RegistrarServer to use protobuf

**Files:**
- Modify: `src/net/registrar_server.cpp:165-276` (handle_tcp_message method)
- Modify: `src/net/registrar_server.cpp:297-353` (broadcast methods)

- [ ] **Step 1: Read current implementation**

Read `src/net/registrar_server.cpp` lines 165-276 to see current manual byte parsing.

- [ ] **Step 2: Replace handle_tcp_message Register case with protobuf**

Replace the manual byte parsing in `TcpMessageType::Register` case with:

```cpp
case TcpMessageType::Register: {
    PbRegisterPayload msg;
    if (!msg.ParseFromArray(data.data(), static_cast<int>(data.size()))) {
        return;
    }

    const auto& ep_info = msg.endpoint_info();
    std::string endpoint_str = ep_info.endpoint();
    CommunicationEndpoint node_endpoint = endpoint_ops::parse_endpoint(endpoint_str);

    if (std::holds_alternative<Ipv4Endpoint>(node_endpoint) &&
        std::get<Ipv4Endpoint>(node_endpoint).is_unspecified()) {
        return;
    }

    std::string client_host = ep_info.host();
    uint16_t client_port = static_cast<uint16_t>(ep_info.tcp_port());

    // Acceptors are at top level of PbRegisterPayload (per spec)
    std::vector<AcceptorInfo> client_acceptors;
    for (const auto& a : msg.acceptors()) {
        AcceptorInfo acceptor;
        acceptor.port = static_cast<uint16_t>(a.port());
        acceptor.handshake_version = static_cast<uint8_t>(a.handshake_version());
        acceptor.protocol_version = static_cast<uint8_t>(a.protocol_version());
        acceptor.tls_required = a.tls_required();
        client_acceptors.push_back(acceptor);
    }

    // Update clients map
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        clients_.erase(conn->remote_endpoint());
        clients_[node_endpoint] = conn;
    }

    // Create and upsert endpoint
    NodeEndpoint ep;
    ep.endpoint = node_endpoint;
    ep.host = client_host;
    ep.tcp_port = client_port;
    ep.acceptors = std::move(client_acceptors);
    ep.last_seen = std::chrono::steady_clock::now();
    registry_.upsert_endpoint(ep);

    // Send Accept response using protobuf
    bytes accept_payload = serialize_accept_payload(0);
    conn->send_message(TcpMessageType::Accept, accept_payload);

    // Broadcast node joined
    broadcast_node_joined(node_endpoint, ep);
    break;
}
```

- [ ] **Step 3: Replace broadcast_node_joined with protobuf**

Replace manual byte encoding with:

```cpp
void RegistrarServer::broadcast_node_joined(CommunicationEndpoint endpoint,
                                            const NodeEndpoint& ep) {
    bytes payload = serialize_node_join_payload(ep);

    std::lock_guard<std::mutex> lock(clients_mutex_);
    for (const auto& [id, conn] : clients_) {
        if (id != endpoint) {
            conn->send_message(TcpMessageType::NodeJoin, payload);
        }
    }
}
```

- [ ] **Step 4: Replace broadcast_node_left with protobuf**

Replace manual byte encoding with:

```cpp
void RegistrarServer::broadcast_node_left(CommunicationEndpoint endpoint) {
    bytes payload = serialize_node_leave_payload(endpoint);

    std::lock_guard<std::mutex> lock(clients_mutex_);
    for (const auto& [id, conn] : clients_) {
        (void)id;
        conn->send_message(TcpMessageType::NodeLeave, payload);
    }
}
```

- [ ] **Step 5: Add include**

Add at top of `src/net/registrar_server.cpp`:
```cpp
#include <hpactor/net/registrar_serialization.hpp>
```

- [ ] **Step 6: Verify build**

Run: `ninja -C build hpactor_lib`
Expected: SUCCESS

- [ ] **Step 7: Commit**

```bash
git add src/net/registrar_server.cpp
git commit -m "refactor(registrar): use protobuf serialization in RegistrarServer"
```

---

## Task 5: Update RegistrarClient to use protobuf

**Files:**
- Modify: `src/net/registrar_client.cpp:232-283` (send_registration method)
- Modify: `src/net/registrar_client.cpp:285-397` (handle_server_message method)

- [ ] **Step 1: Read current implementation**

Read `src/net/registrar_client.cpp` lines 232-397 to see current manual byte building and parsing.

- [ ] **Step 2: Replace send_registration with protobuf**

Replace `send_registration()` method body with:

```cpp
void RegistrarClient::send_registration() {
    if (!server_connection_ || !connected_.load()) {
        return;
    }

    // Build registration message using protobuf
    std::string host = get_local_ip();
    uint16_t tcp_port = config_.tcp_port;

    // Create NodeEndpoint for serialization
    NodeEndpoint ep;
    ep.endpoint = local_endpoint_;
    ep.host = host;
    ep.tcp_port = tcp_port;
    ep.acceptors = acceptors_;

    bytes payload = serialize_register_payload(ep);
    server_connection_->send_message(TcpMessageType::Register, payload);
}
```

- [ ] **Step 3: Replace handle_server_message NodeJoin parsing**

Replace the manual byte parsing in `TcpMessageType::NodeJoin` case with:

```cpp
case TcpMessageType::NodeJoin: {
    PbNodeJoinPayload msg;
    if (!msg.ParseFromArray(data.data(), static_cast<int>(data.size()))) {
        break;
    }

    const auto& ep_info = msg.endpoint_info();
    std::string endpoint_str = ep_info.endpoint();
    CommunicationEndpoint endpoint = endpoint_ops::parse_endpoint(endpoint_str);

    std::string host = ep_info.host();
    uint16_t tcp_port = static_cast<uint16_t>(ep_info.tcp_port());

    NodeEndpoint node_ep;
    node_ep.endpoint = endpoint;
    node_ep.host = host;
    node_ep.tcp_port = tcp_port;
    node_ep.last_seen = std::chrono::steady_clock::now();
    shared_registry_->upsert_endpoint(node_ep);
    break;
}
```

- [ ] **Step 4: Replace handle_server_message NodeLeave parsing**

Replace the manual byte parsing in `TcpMessageType::NodeLeave` case with:

```cpp
case TcpMessageType::NodeLeave: {
    PbNodeLeavePayload msg;
    if (!msg.ParseFromArray(data.data(), static_cast<int>(data.size()))) {
        break;
    }

    std::string endpoint_str = msg.endpoint();
    CommunicationEndpoint endpoint = endpoint_ops::parse_endpoint(endpoint_str);
    shared_registry_->remove_endpoint(endpoint);
    break;
}
```

- [ ] **Step 5: Add include**

Add at top of `src/net/registrar_client.cpp`:
```cpp
#include <hpactor/net/registrar_serialization.hpp>
```

- [ ] **Step 6: Verify build**

Run: `ninja -C build hpactor_lib`
Expected: SUCCESS

- [ ] **Step 7: Commit**

```bash
git add src/net/registrar_client.cpp
git commit -m "refactor(registrar): use protobuf serialization in RegistrarClient"
```

---

## Task 6: Implement async UDP in UdpRegistrar

**Files:**
- Modify: `src/net/registrar.cpp` (UdpRegistrar methods)
- Modify: `include/hpactor/net/registrar.hpp` (UdpRegistrar class)

- [ ] **Step 1: Read current UDP implementation**

Read `src/net/registrar.cpp` lines 420-720 to see current `handle_udp_packet` and UDP socket usage.

- [ ] **Step 2: Add async UDP methods to UdpRegistrar class**

In `include/hpactor/net/registrar.hpp`, add to `UdpRegistrar` class (around line 376):

```cpp
private:
    void start_server_mode_async();
    void start_client_mode_async();
    void issue_async_recvfrom();
    void handle_udp_recv_completion(const bytes& data, const std::string& from_host, uint16_t from_port);
    void send_udp_response(const bytes& data, const struct sockaddr_in& dest);

    // UDP receive state
    static constexpr size_t kUdpRecvBufferSize = 65536;
    bytes udp_recv_buffer_;
    struct sockaddr_in udp_src_addr_;
    socklen_t udp_src_addr_len_ = sizeof(udp_src_addr_);
```

- [ ] **Step 3: Implement async UDP methods**

Add to `src/net/registrar.cpp`:

```cpp
void UdpRegistrar::start_server_mode_async() {
    server_ = std::make_unique<RegistrarServer>(config_, local_endpoint_, loop_);
    server_->start();

    // Create UDP socket
    udp_socket_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_socket_ < 0) return;

    // Bind to UDP port
    struct sockaddr_in udp_addr;
    memset(&udp_addr, 0, sizeof(udp_addr));
    udp_addr.sin_family = AF_INET;
    udp_addr.sin_addr.s_addr = INADDR_ANY;
    udp_addr.sin_port = htons(config_.udp_port);
    bind(udp_socket_, reinterpret_cast<struct sockaddr*>(&udp_addr), sizeof(udp_addr));

    // Register with EventLoop for read events
    if (loop_) {
        loop_->add_fd(udp_socket_, EventLoop::Event::Read);
    }

    // Allocate receive buffer
    udp_recv_buffer_.resize(kUdpRecvBufferSize);

    // Issue first async recvfrom
    issue_async_recvfrom();
}

void UdpRegistrar::start_client_mode_async() {
    // Create registry populated with static routes
    client_registry_ = std::make_unique<NodeRegistry>(config_);

    // Populate with static routes
    for (const auto& route : config_.static_routes) {
        NodeEndpoint ep;
        ep.endpoint = route.endpoint;
        ep.host = route.address;
        ep.tcp_port = route.port;
        ep.is_static_route = true;
        client_registry_->upsert_endpoint(ep);
    }

    // Use first static route as server if available
    CommunicationEndpoint server_endpoint;
    if (!config_.static_routes.empty()) {
        server_endpoint = config_.static_routes[0].endpoint;
    }

    // Create UDP socket
    udp_socket_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_socket_ >= 0 && loop_) {
        loop_->add_fd(udp_socket_, EventLoop::Event::Read);
        udp_recv_buffer_.resize(kUdpRecvBufferSize);
        issue_async_recvfrom();
    }

    client_ = std::make_unique<RegistrarClient>(config_, local_endpoint_, server_endpoint, client_registry_.get(), loop_);
    client_->start();
}

void UdpRegistrar::issue_async_recvfrom() {
    if (udp_socket_ < 0 || !loop_) return;

    // Clear address storage for next recvfrom
    memset(&udp_src_addr_, 0, sizeof(udp_src_addr_));
    udp_src_addr_len_ = sizeof(udp_src_addr_);
}

void UdpRegistrar::handle_udp_read_ready() {
    if (udp_socket_ < 0 || !loop_) return;

    // Check if UDP socket is readable (edge-triggered)
    if (!loop_->has_event(udp_socket_, EventLoop::Event::Read)) {
        return;
    }

    // Do non-blocking recvfrom
    char buffer[kUdpRecvBufferSize];
    struct sockaddr_in src_addr;
    socklen_t src_addr_len = sizeof(src_addr);

    ssize_t bytes_read = recvfrom(udp_socket_, buffer, sizeof(buffer), 0,
                                  reinterpret_cast<struct sockaddr*>(&src_addr),
                                  &src_addr_len);

    if (bytes_read > 0) {
        bytes data(buffer, buffer + bytes_read);
        char ip_str[INET_ADDRSTRLEN];
        std::string from_host;
        uint16_t from_port = 0;

        if (inet_ntop(AF_INET, &src_addr.sin_addr, ip_str, sizeof(ip_str))) {
            from_host = ip_str;
        }
        from_port = ntohs(src_addr.sin_port);

        handle_udp_recv_completion(data, from_host, from_port);
    }
}

void UdpRegistrar::handle_udp_recv_completion(const bytes& data, const std::string& from_host, uint16_t from_port) {
    // Call the existing handler
    handle_udp_packet(data, from_host, from_port);
}

void UdpRegistrar::send_udp_response(const bytes& data, const struct sockaddr_in& dest) {
    if (udp_socket_ < 0) return;

    if (loop_) {
        // Use async_sendto for async UDP send
        struct iovec iov;
        iov.iov_base = const_cast<uint8_t*>(data.data());
        iov.iov_len = data.size();

        loop_->backend()->async_sendto(
            udp_socket_,
            &iov,
            1,
            reinterpret_cast<const sockaddr*>(&dest),
            sizeof(dest),
            ActorId(0),
            static_cast<uint32_t>(OpType::SendTo));
    } else {
        // Fallback to blocking sendto
        sendto(udp_socket_, data.data(), data.size(), 0,
               reinterpret_cast<const struct sockaddr*>(&dest), sizeof(dest));
    }
}
```

- [ ] **Step 4: Update handle_udp_packet to use protobuf**

Replace manual byte parsing in `handle_udp_packet` with protobuf parsing:

```cpp
void UdpRegistrar::handle_udp_packet(const bytes& data, const std::string& from_host, uint16_t from_port) {
    if (data.size() < RegistrarHeaderSize) {
        return;
    }

    // Parse header
    uint32_t magic;
    memcpy(&magic, data.data(), 4);
    magic = ntohl(magic);

    if (magic != RegistrarMagic) {
        return;
    }

    uint8_t version = data[4];
    if (version != RegistrarVersion) {
        return;
    }

    RegistrarMessageType type = static_cast<RegistrarMessageType>(data[5]);

    uint32_t payload_len;
    memcpy(&payload_len, data.data() + 6, 4);
    payload_len = ntohl(payload_len);

    if (data.size() < RegistrarHeaderSize + payload_len) {
        return;
    }

    const bytes payload(data.begin() + RegistrarHeaderSize, data.begin() + RegistrarHeaderSize + payload_len);

    switch (type) {
        case RegistrarMessageType::ResolveQuery: {
            PbResolveQueryPayload msg = parse_resolve_query_payload(payload);
            std::string target_str = msg.target_endpoint();
            CommunicationEndpoint target_endpoint = endpoint_ops::parse_endpoint(target_str);

            if (server_) {
                NodeEndpoint* ep = server_->registry()->get(target_endpoint);
                if (ep) {
                    bytes response_payload = serialize_resolve_response_payload(*ep);

                    // Build UDP header
                    bytes response;
                    response.resize(RegistrarHeaderSize + response_payload.size());
                    uint32_t magic_be = htonl(RegistrarMagic);
                    memcpy(response.data(), &magic_be, 4);
                    response[4] = RegistrarVersion;
                    response[5] = static_cast<uint8_t>(RegistrarMessageType::ResolveResponse);
                    uint32_t len_be = htonl(static_cast<uint32_t>(response_payload.size()));
                    memcpy(response.data() + 6, &len_be, 4);
                    memcpy(response.data() + RegistrarHeaderSize, response_payload.data(), response_payload.size());

                    // Send response
                    struct sockaddr_in dest;
                    memset(&dest, 0, sizeof(dest));
                    dest.sin_family = AF_INET;
                    dest.sin_port = htons(from_port);
                    inet_pton(AF_INET, from_host.c_str(), &dest.sin_addr);

                    send_udp_response(response, dest);
                }
            }
            break;
        }

        case RegistrarMessageType::ResolveResponse: {
            PbResolveResponsePayload msg = parse_resolve_response_payload(payload);
            const auto& ep_info = msg.endpoint_info();

            std::string endpoint_str = ep_info.endpoint();
            CommunicationEndpoint resp_endpoint = endpoint_ops::parse_endpoint(endpoint_str);
            std::string host = ep_info.host();
            uint16_t tcp_port = static_cast<uint16_t>(ep_info.tcp_port());

            if (server_) {
                NodeEndpoint ep;
                ep.endpoint = resp_endpoint;
                ep.host = host;
                ep.tcp_port = tcp_port;
                ep.last_seen = std::chrono::steady_clock::now();
                server_->registry()->upsert_endpoint(ep);

                if (node_callback_) {
                    node_callback_(resp_endpoint, true);
                }
            }
            break;
        }
    }
}
```

- [ ] **Step 5: Add include**

Add at top of `src/net/registrar.cpp`:
```cpp
#include <hpactor/net/registrar_serialization.hpp>
```

- [ ] **Step 6: Verify build**

Run: `ninja -C build hpactor_lib`
Expected: SUCCESS (may have warnings about unused variables)

- [ ] **Step 7: Commit**

```bash
git add src/net/registrar.cpp include/hpactor/net/registrar.hpp
git commit -m "feat(registrar): add async UDP support via EventLoop"
```

---

## Task 7: Add serialization tests

**Files:**
- Create: `tests/net/test_registrar_serialization.cpp`

- [ ] **Step 1: Write round-trip and malformed data tests**

```cpp
// tests/net/test_registrar_serialization.cpp
// Copyright 2026 HPActor Contributors

#include <hpactor/net/registrar_serialization.hpp>
#include <hpactor/net/registrar.hpp>

#include <cassert>
#include <iostream>
#include <cstring>

using namespace hpactor;
using namespace hpactor::net;

int main() {
    // Test 1: PbRegisterPayload round-trip (acceptors at top level per spec)
    {
        NodeEndpoint ep;
        ep.endpoint = endpoint_ops::parse_endpoint("192.168.1.100:5353");
        ep.host = "192.168.1.100";
        ep.tcp_port = 5353;
        ep.acceptors.push_back({8080, 1, 1, false});

        bytes serialized = serialize_register_payload(ep);
        PbRegisterPayload parsed = parse_register_payload(serialized);

        assert(parsed.endpoint_info().endpoint() == "192.168.1.100:5353");
        assert(parsed.endpoint_info().host() == "192.168.1.100");
        assert(parsed.endpoint_info().tcp_port() == 5353);
        assert(parsed.acceptors_size() == 1);
        assert(parsed.acceptors(0).port() == 8080);
    }

    // Test 2: PbAcceptPayload round-trip
    {
        bytes serialized = serialize_accept_payload(0);
        PbAcceptPayload parsed = parse_accept_payload(serialized);
        assert(parsed.error_code() == 0);
    }

    // Test 3: PbNodeJoinPayload round-trip
    {
        NodeEndpoint ep;
        ep.endpoint = endpoint_ops::parse_endpoint("10.0.0.1:4000");
        ep.host = "10.0.0.1";
        ep.tcp_port = 4000;

        bytes serialized = serialize_node_join_payload(ep);
        PbNodeJoinPayload parsed = parse_node_join_payload(serialized);

        assert(parsed.endpoint_info().endpoint() == "10.0.0.1:4000");
        assert(parsed.endpoint_info().host() == "10.0.0.1");
        assert(parsed.endpoint_info().tcp_port() == 4000);
    }

    // Test 4: PbNodeLeavePayload round-trip
    {
        auto ep = endpoint_ops::parse_endpoint("10.0.0.2:5000");
        bytes serialized = serialize_node_leave_payload(ep);
        PbNodeLeavePayload parsed = parse_node_leave_payload(serialized);
        assert(parsed.endpoint() == "10.0.0.2:5000");
    }

    // Test 5: PbResolveQueryPayload round-trip
    {
        auto ep = endpoint_ops::parse_endpoint("192.168.1.50:5353");
        bytes serialized = serialize_resolve_query_payload(ep);
        PbResolveQueryPayload parsed = parse_resolve_query_payload(serialized);
        assert(parsed.target_endpoint() == "192.168.1.50:5353");
    }

    // Test 6: PbResolveResponsePayload round-trip (endpoint_info only, no acceptors per spec)
    {
        NodeEndpoint ep;
        ep.endpoint = endpoint_ops::parse_endpoint("192.168.1.50:5353");
        ep.host = "192.168.1.50";
        ep.tcp_port = 5353;

        bytes serialized = serialize_resolve_response_payload(ep);
        PbResolveResponsePayload parsed = parse_resolve_response_payload(serialized);

        assert(parsed.endpoint_info().endpoint() == "192.168.1.50:5353");
        assert(parsed.endpoint_info().host() == "192.168.1.50");
        assert(parsed.endpoint_info().tcp_port() == 5353);
    }

    // Test 7: Malformed data handling
    {
        bytes malformed = {0x00, 0x01, 0x02};  // Too short
        PbRegisterPayload parsed = parse_register_payload(malformed);
        // ParseFromArray returns false on error, message should be empty/default
        assert(parsed.endpoint_info().endpoint().empty());
    }

    std::cout << "All registrar_serialization tests passed" << std::endl;
    return 0;
}
```

- [ ] **Step 2: Add test to CMakeLists.txt**

Find the net tests section in `tests/net/CMakeLists.txt` and add:

```cmake
add_executable(test_registrar_serialization test_registrar_serialization.cpp)
target_link_libraries(test_registrar_serialization hpactor_lib hpactor_proto pthread)
```

- [ ] **Step 3: Build and run test**

Run: `ninja -C build test_registrar_serialization && ./build/tests/net/test_registrar_serialization`
Expected: "All registrar_serialization tests passed"

- [ ] **Step 4: Commit**

```bash
git add tests/net/test_registrar_serialization.cpp tests/net/CMakeLists.txt
git commit -m "test(registrar): add round-trip and malformed data tests for protobuf serialization"
```

---

## Verification

After all tasks complete, run the full test suite:

```bash
ctest --output-on-failure
```

Expected: All tests pass including the new `test_registrar_serialization`.
