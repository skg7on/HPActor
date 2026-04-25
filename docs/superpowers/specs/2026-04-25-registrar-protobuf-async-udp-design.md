# Registrar Protobuf Serialization and Async UDP Design

**Date:** 2026-04-25
**Status:** Design
**Author:** HPActor Team

## Context

The existing `RegistrarServer` and `RegistrarClient` use manual byte serialization (`memcpy`, `htonl`, `ntohl`) for all TCP messages, and `UdpRegistrar` uses blocking `sendto`/`recvfrom` for UDP. This design covers:

1. **New `registrar.proto`** with protobuf definitions for all registrar TCP and UDP message types
2. **Async UDP** via `AsyncIoBackend::async_recvfrom`/`async_sendto` integrated with EventLoop

## Part 1: Registrar Proto Definition

### File Location

Create `protos/hpactor/registrar.proto` (separate from `messages.proto` which contains actor system messages).

### Proto Schema

```protobuf
syntax = "proto3";

package hpactor;

option optimize_for = SPEED;

// -----------------------------------------------------------------------------
// Common Types
// -----------------------------------------------------------------------------

message PbEndpointInfo {
    string endpoint = 1;  // Serialized CommunicationEndpoint
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
// TCP Messages ( RegistrarConnection protocol)
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

// TCP Heartbeat has no payload

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

### Message Type Mapping

| TCP Type | Proto Message | UDP Type | Proto Message |
|----------|---------------|----------|---------------|
| Register | `PbRegisterPayload` | ResolveQuery | `PbResolveQueryPayload` |
| Accept | `PbAcceptPayload` | ResolveResponse | `PbResolveResponsePayload` |
| Heartbeat | (empty) | | |
| NodeJoin | `PbNodeJoinPayload` | | |
| NodeLeave | `PbNodeLeavePayload` | | |
| Error | `PbErrorPayload` | | |

## Part 2: Protobuf Serialization Helpers

### File Location

Create `src/net/registrar_serialization.cpp` with `to_proto`/`from_proto` helpers following the pattern in `src/core/serialization.cpp`.

### Helper Pattern

```cpp
// src/net/registrar_serialization.hpp

#pragma once

#include <hpactor/net/registrar.hpp>
#include <hpactor/serialization.hpp>
#include <hpactor/net/registrar.pb.h>

namespace hpactor {
namespace net {

// TCP message to_proto helpers
inline PbRegisterPayload to_proto(const NodeEndpoint& ep) {
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
    PbRegisterPayload msg = to_proto(ep);
    return bytes(msg.SerializeAsString());
}

// ... similar helpers for other message types
```

### Integration with RegistrarConnection

The `RegistrarConnection::send_message()` already takes raw bytes. We just need to serialize the protobuf before calling:

```cpp
void RegistrarServer::handle_tcp_message(RegistrarConnectionPtr conn,
                                         TcpMessageType type,
                                         const bytes& data) {
    switch (type) {
        case TcpMessageType::Register: {
            PbRegisterPayload msg;
            if (!msg.ParseFromArray(data.data(), static_cast<int>(data.size()))) {
                return;
            }
            // Use msg.endpoint_info(), msg.acceptors()
            break;
        }
        // ... other cases
    }
}
```

## Part 3: Async UDP via EventLoop

### Current State

`UdpRegistrar` creates a UDP socket in `start_server_mode()` and `start_client_mode()` but:
1. The socket is created with raw `socket()` - not registered with EventLoop
2. `handle_udp_packet()` is called synchronously - no async I/O
3. Responses use blocking `sendto()`

### Target State

UDP socket registered with EventLoop, using `AsyncIoBackend::async_recvfrom` and `async_sendto`.

### AsyncIoBackend Interface

From `async_io_backend.hpp`:

```cpp
class AsyncIoBackend {
public:
    // UDP operations
    virtual void async_recvfrom(int fd, void* buf, size_t len, ActorId actor,
                                uint32_t op_flags) = 0;
    virtual void async_sendto(int fd, const void* buf, size_t len,
                              const struct sockaddr* dest, socklen_t addrlen,
                              ActorId actor, uint32_t op_flags) = 0;
    // ...
};
```

### Async UdpRegistrar Design

```cpp
// In include/hpactor/net/registrar.hpp (UdpRegistrar class)

class UdpRegistrar {
public:
    // ... existing methods ...

private:
    void start_server_mode_async();
    void start_client_mode_async();
    void handle_udp_recv_completion(int result, struct sockaddr_in* src_addr,
                                     socklen_t addr_len, bytes received_data);
    void send_udp_response(const bytes& data, const struct sockaddr_in& dest);

    // UDP receive state
    static constexpr size_t UdpRecvBufferSize = 65536;
    bytes udp_recv_buffer_;
    struct sockaddr_in udp_src_addr_;
    socklen_t udp_src_addr_len_;
};
```

### UDP Read Flow

```cpp
void UdpRegistrar::start_server_mode_async() {
    server_ = std::make_unique<RegistrarServer>(config_, local_endpoint_, loop_);
    server_->start();

    // Create UDP socket
    udp_socket_ = socket(AF_INET, SOCK_DGRAM, 0);
    // ... bind ...

    // Register with EventLoop
    loop_->add_fd(udp_socket_, EventLoop::Event::Read);

    // Allocate receive buffer
    udp_recv_buffer_.resize(UdpRecvBufferSize);

    // Issue first async recvfrom
    issue_async_recvfrom();
}

void UdpRegistrar::issue_async_recvfrom() {
    if (udp_socket_ < 0 || !loop_) return;

    // Prepare address storage
    udp_src_addr_len_ = sizeof(udp_src_addr_);
    memset(&udp_src_addr_, 0, sizeof(udp_src_addr_));

    loop_->backend()->async_recvfrom(
        udp_socket_,
        udp_recv_buffer_.data(),
        UdpRecvBufferSize,
        ActorId(0),  // actor id for completion routing
        0);
}

void UdpRegistrar::handle_udp_recv_completion(int result, struct sockaddr_in* src_addr,
                                                socklen_t addr_len, bytes received_data) {
    if (result > 0) {
        // Process received packet
        bytes packet(received_data.begin(), received_data.begin() + result);
        std::string from_host;
        uint16_t from_port = 0;

        if (src_addr && addr_len >= sizeof(struct sockaddr_in)) {
            char ip_str[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &src_addr->sin_addr, ip_str, sizeof(ip_str));
            from_host = ip_str;
            from_port = ntohs(src_addr->sin_port);
        }

        // Handle packet (parse header, dispatch to handler)
        handle_udp_packet(packet, from_host, from_port);
    }

    // Issue next recv to keep reading
    issue_async_recvfrom();
}
```

### UDP Write Flow

```cpp
void UdpRegistrar::send_udp_response(const bytes& data, const struct sockaddr_in& dest) {
    if (udp_socket_ < 0 || !loop_) return;

    loop_->backend()->async_sendto(
        udp_socket_,
        data.data(),
        data.size(),
        reinterpret_cast<const struct sockaddr*>(&dest),
        sizeof(dest),
        ActorId(0),
        0);
    // Note: send completion is best-effort, we don't wait for it
}
```

### EventLoop Completion Routing for UDP

The `EventLoop::completion_callback_` routes completions by `OpType` and fd. For UDP, we need to:

1. Register a completion handler that knows about UDP socket
2. Route `OpType::RecvFrom` and `OpType::SendTo` appropriately

```cpp
// In RegistrarServer::start() or where EventLoop is set up:

loop_->set_completion_callback([this](OpCompletion c) {
    if (c.fd == udp_socket_) {
        if (c.type == OpType::RecvFrom) {
            // UDP receive completion
            // Extract src address from stored state
            // Call handle_udp_recv_completion
        } else if (c.type == OpType::SendTo) {
            // UDP send completion (best-effort, ignore errors)
        }
    }
});
```

### Completion Callback Storage

The `OpCompletion` struct needs to carry the source address for `recvfrom`. We need to extend it:

```cpp
// In event_loop.hpp - OpCompletion struct
struct OpCompletion {
    OpType type;
    int fd;
    int result;
    ActorId actor;

    // For recvfrom: source address info
    struct sockaddr_in src_addr;
    socklen_t src_addr_len;
};
```

The backend stores the source address when completing `async_recvfrom`.

## File Changes

| File | Change |
|------|--------|
| `protos/hpactor/registrar.proto` | **Create** - protobuf definitions for registrar messages |
| `protos/hpactor/CMakeLists.txt` | **Add** registrar.proto |
| `src/net/registrar_serialization.cpp` | **Create** - protobuf serialization helpers |
| `src/net/registrar_serialization.hpp` | **Create** - header for helpers |
| `include/hpactor/net/registrar.hpp` | **Modify** - extend UdpRegistrar with async methods |
| `src/net/registrar.cpp` | **Modify** - implement async UDP methods |
| `include/hpactor/event_loop.hpp` | **Modify** - extend `OpCompletion` with src_addr |
| `src/net/registrar_server.cpp` | **Modify** - use protobuf for message parsing |
| `src/net/registrar_client.cpp` | **Modify** - use protobuf for message building |

## Implementation Order

1. **Phase 1:** Add `src_addr` fields to `OpCompletion` struct
2. **Phase 2:** Create `protos/hpactor/registrar.proto` and generate headers
3. **Phase 3:** Create `registrar_serialization.cpp/hpp` with to_proto/from_proto helpers
4. **Phase 4:** Update `RegistrarConnection` message handlers to use protobuf
5. **Phase 5:** Implement async UDP in `UdpRegistrar` using `async_recvfrom`/`async_sendto`
6. **Phase 6:** Update `handle_udp_packet` to parse protobuf instead of manual byte parsing

## Testing

1. **Round-trip test:** Serialize PbRegisterPayload → deserialize → verify fields match
2. **Malformed data test:** Feed invalid bytes → verify graceful handling
3. **Async UDP test:** Send UDP packet → verify handle_udp_packet called with correct data
4. **Integration test:** Two-process test with registrar using protobuf + async UDP
