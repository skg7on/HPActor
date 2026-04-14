# Registrar Refactor Design

**Date:** 2026-04-14
**Status:** Draft
**Owner:** SKG7ON
**Type:** Bug Fix / Feature Implementation

## Overview

Fix critical bugs in the service discovery registrar implementation and complete incomplete features. The registrar provides distributed node registration and name resolution for the HPActor actor framework.

---

## Issue 1: Server handle_tcp_message() Doesn't Process Register Messages

**Location:** `src/net/registrar_server.cpp:234-239`

**Problem:** The `handle_tcp_message()` method is a stub that ignores all message data:
```cpp
void RegistrarServer::handle_tcp_message(int client_fd, const bytes& data) {
    (void)client_fd;
    (void)data;
}
```

**Required Fix:** Parse incoming TCP messages and handle Register, Heartbeat, and other types:

1. Parse message type from header (byte 5)
2. For `Register` (0x01):
   - Parse payload: `[NodeId: 4][HostLen: 1][Host: N][Port: 2][AcceptorCount: 1][Acceptors: ...]`
   - Validate NodeId is non-zero
   - Add entry to `clients_` map: `clients_[node_id] = client_fd`
   - Create NodeEndpoint and upsert to registry
   - Send `Accept` response: `[ErrorCode: 1]`
   - Broadcast `NodeJoin` to all other clients via `broadcast_node_joined()`
3. For `Heartbeat` (0x02):
   - Update `last_seen` for the client in registry
4. For unknown types:
   - Log warning (when logging is added)

**Payload Format for Register:**
```
[NodeId: 4][HostLen: 1][Host: N][TcpPort: 2][AcceptorCount: 1][Acceptors: ...]
```

---

## Issue 2: Client FD Storage — clients_ Map Never Populated

**Location:** `src/net/registrar_server.cpp:144-232`

**Problem:** In `handle_accept()`, after accepting a connection, the `client_fd` is never added to `clients_` map. The map is only modified when removing clients (lines 223-229).

**Required Fix:** Add client to `clients_` map when a new connection is accepted:

1. After successful accept at line 137-138:
2. Read the initial `Register` message from the client
3. Parse the `NodeId` from the Register payload
4. Add `clients_[node_id] = client_fd` before entering the read loop
5. If client doesn't send valid Register within timeout, close connection

**Sequence:**
```cpp
void RegistrarServer::handle_accept(int client_fd) {
    // Set TCP_NODELAY...
    // Read and parse Register message (blocking read for first message only)
    // If Register received with valid NodeId:
    //   clients_[node_id] = client_fd;
    //   registry_->upsert_endpoint(ep);
    //   send Accept
    //   broadcast_node_joined()
    // Then enter the existing message loop...
}
```

---

## Issue 3: Null Pointer Issue — RegistrarClient Receives nullptr for shared_registry

**Location:** `src/net/registrar.cpp:230`

**Problem:**
```cpp
client_ = std::make_unique<RegistrarClient>(config_, local_node_id_, server_node_id, nullptr, loop_);
```

The `UdpRegistrar::start_client_mode()` passes `nullptr` for `shared_registry`, but `RegistrarClient::connect_to_server()` at line 129 dereferences it:
```cpp
NodeEndpoint* server_ep = shared_registry_->get(server_node_id_);
```

**Required Fix:** Client mode needs access to the server's registry to look up the server's endpoint. Options:

1. **Option A (Preferred):** Store a local `NodeRegistry` in `UdpRegistrar` that the client can use. When in client mode, use static routes to find the server.

2. **Option B:** Pass a pointer to the static route configuration so client can resolve server address directly without registry lookup.

**Implementation (Option A):**
- Add `std::unique_ptr<NodeRegistry> client_registry_` to `UdpRegistrar`
- In `start_client_mode()`, populate `client_registry_` with static routes
- Pass `client_registry_.get()` to `RegistrarClient`

```cpp
void UdpRegistrar::start_client_mode() {
    client_registry_ = std::make_unique<NodeRegistry>(config_);

    // Populate with static routes
    for (const auto& route : config_.static_routes) {
        NodeEndpoint ep;
        ep.node_id = route.node_id;
        ep.host = route.address;
        ep.tcp_port = route.port;
        ep.is_static_route = true;
        client_registry_->upsert_endpoint(ep);
    }

    NodeId server_node_id = 0;
    if (!config_.static_routes.empty()) {
        server_node_id = config_.static_routes[0].node_id;
    }

    client_ = std::make_unique<RegistrarClient>(config_, local_node_id_, server_node_id, client_registry_.get(), loop_);
    client_->start();
}
```

---

## Issue 4: UDP Response — handle_udp_packet() Doesn't Send ResolveResponse

**Location:** `src/net/registrar.cpp:360-363`

**Problem:** When handling `ResolveQuery`, the code builds a response but never sends it:
```cpp
// Would send response back to from_host:from_port via UDP
(void)response;
(void)from_host;
(void)from_port;
```

**Required Fix:** Actually send the UDP response:

1. Get the UDP socket - need to store it as a member or get it from server
2. Use `sendto()` to send response to `from_host:from_port`

**Complication:** The `handle_udp_packet()` is a method on `UdpRegistrar`, not `RegistrarServer`. In server mode, the server doesn't have direct access to a UDP socket for responses.

**Solution:**
1. Store `udp_socket_` as a member of `UdpRegistrar` (not just `RegistrarServer`)
2. In server mode, bind UDP socket in `UdpRegistrar::start_server_mode()`
3. Use that socket to send responses

```cpp
void UdpRegistrar::start_server_mode() {
    server_ = std::make_unique<RegistrarServer>(config_, local_node_id_, loop_);
    server_->start();

    // Create UDP socket for sending responses
    udp_socket_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_socket_ >= 0) {
        // Don't bind - just use for sending
        // Response destination is determined by recvfrom
    }
}

void UdpRegistrar::handle_udp_packet(const bytes& data, const std::string& from_host, uint16_t from_port) {
    // ... existing parsing code ...

    if (type == RegistrarMessageType::ResolveQuery && ep) {
        // ... build response ...

        if (udp_socket_ >= 0) {
            struct sockaddr_in dest_addr;
            memset(&dest_addr, 0, sizeof(dest_addr));
            dest_addr.sin_family = AF_INET;
            dest_addr.sin_port = htons(from_port);
            inet_pton(AF_INET, from_host.c_str(), &dest_addr.sin_addr);

            sendto(udp_socket_, response.data(), response.size(), 0,
                   reinterpret_cast<struct sockaddr*>(&dest_addr), sizeof(dest_addr));
        }
    }
}
```

**Alternative:** The UDP socket creation and response sending should be in `RegistrarServer` itself, since server mode is where resolution responses originate.

---

## Issue 5: Hardcoded "127.0.0.1" — Won't Work Across Machines

**Location:** `src/net/registrar_client.cpp:193`

**Problem:**
```cpp
std::string host = "127.0.0.1";  // In production, get actual local IP
```

**Required Fix:** Get the actual local IP address for the machine:

**Approach:** Query the local hostname and resolve it, or enumerate network interfaces.

```cpp
#include <ifaddrs.h>
#include <net/if.h>

std::string get_local_ip() {
    struct ifaddrs* ifaddr = nullptr;
    if (getifaddrs(&ifaddr) == -1) {
        return "127.0.0.1";  // Fallback
    }

    // Prefer non-loopback, non-down interfaces
    std::string result;
    for (struct ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == nullptr) continue;
        if (!(ifa->ifa_flags & IFF_UP)) continue;
        if (!(ifa->ifa_flags & IFF_RUNNING)) continue;
        if (ifa->ifa_flags & IFF_LOOPBACK) continue;
        if (ifa->ifa_addr->sa_family != AF_INET) continue;

        struct sockaddr_in* addr = reinterpret_cast<struct sockaddr_in*>(ifa->ifa_addr);
        char ip[INET_ADDRSTRLEN];
        if (inet_ntop(AF_INET, &addr->sin_addr, ip, sizeof(ip)) != nullptr) {
            result = ip;
            break;  // Take first valid non-loopback
        }
    }

    freeifaddrs(ifaddr);
    return result.empty() ? "127.0.0.1" : result;
}
```

---

## Issue 6: Registration Missing AcceptorInfo in Payload

**Location:** `src/net/registrar_client.cpp:186-225`

**Problem:** The registration payload format is:
```
[NodeId: 4][HostLen: 1][Host: N][TcpPort: 2]
```

But `NodeEndpoint` has an `acceptors` field that should be announced.

**Required Fix:** Extend registration payload format:

```
[NodeId: 4][HostLen: 1][Host: N][TcpPort: 2][AcceptorCount: 1][Acceptors: ...]
```

Where each `AcceptorInfo` is:
```
[Port: 2][HandshakeVersion: 1][ProtocolVersion: 1][TlsRequired: 1]
```

**Implementation:**
1. Add `acceptors` parameter to `send_registration()` or make it a member
2. Serialize AcceptorInfo array after TcpPort

```cpp
void RegistrarClient::send_registration() {
    // ... existing NodeId, Host, TcpPort serialization ...

    // Add Acceptors
    uint8_t acceptor_count = static_cast<uint8_t>(acceptors_.size());
    payload[offset++] = acceptor_count;

    for (const auto& acceptor : acceptors_) {
        uint16_t port_be = htons(acceptor.port);
        memcpy(payload.data() + offset, &port_be, 2);
        offset += 2;

        payload[offset++] = acceptor.handshake_version;
        payload[offset++] = acceptor.protocol_version;
        payload[offset++] = acceptor.tls_required ? 1 : 0;
    }

    // ... rest of message building ...
}
```

---

## Issue 7: Failover Reconnect Logic — Stubbed

**Location:** `src/net/registrar_client.cpp:373-474`

**Problem:** The `reconnect()` and `failover()` methods have partial UDP broadcast code but it's incomplete and not wired up properly.

**Required Fix:** Complete failover implementation:

1. **Detection:** Connection loss triggers failover (on read error, heartbeat timeout)
2. **Election:** Try to bind TCP port to become server
3. **Discovery:** If can't become server, use UDP broadcast to find existing servers
4. **Reconnect:** Connect to found server

**State Machine:**
```
CONNECTED -> [detect disconnect] -> DISCONNECTED
DISCONNECTED -> [try bind port] -> SERVER_MODE
                       |
                       v (bind fails)
                -> [broadcast probe] -> WAITING_FOR_SERVER
                                          |
                                          v (receive response)
                                    CONNECTED (new server)
```

**Implementation:**

```cpp
void RegistrarClient::handle_connection_lost() {
    connected_.store(false);

    // Stop heartbeat thread
    if (heartbeat_thread_.joinable()) {
        heartbeat_thread_.join();
    }

    // Try to become server
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock >= 0) {
        int reuse = 1;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(config_.tcp_port);

        if (bind(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0) {
            // Won! Become server - signal to UdpRegistrar
            close(sock);
            // TODO: Signal UdpRegistrar to transition to server mode
            return;
        }
        close(sock);
    }

    // Can't become server - try to find one via broadcast
    find_server_via_broadcast();
}

void RegistrarClient::find_server_via_broadcast() {
    // Create UDP socket for broadcast
    int broadcast_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (broadcast_sock < 0) return;

    int broadcast_enable = 1;
    setsockopt(broadcast_sock, SOL_SOCKET, SO_BROADCAST, &broadcast_enable, sizeof(broadcast_enable));

    struct sockaddr_in broadcast_addr;
    memset(&broadcast_addr, 0, sizeof(broadcast_addr));
    broadcast_addr.sin_family = AF_INET;
    broadcast_addr.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    broadcast_addr.sin_port = htons(config_.udp_port);

    // Send probe
    // Format: [Magic: 4][Version: 1][Type: 1][Length: 4][ProbeId: 8][Timestamp: 8]
    // TODO: Define Probe message type

    // Wait for response with timeout
    // Parse response, extract server IP:port
    // Update shared_registry_ with server endpoint
    // Set server_node_id_ to new server
    // Attempt connect_to_server()

    close(broadcast_sock);
}
```

**Critical:** Heartbeat timeout must trigger `handle_connection_lost()`. Add timeout tracking in `heartbeat_loop()` or use `EventLoop` timers with expiry callback.

---

## File Changes Summary

| File | Changes |
|------|---------|
| `src/net/registrar_server.cpp` | Implement `handle_tcp_message()`, add client to `clients_` map on accept |
| `src/net/registrar_client.cpp` | Fix null pointer, get local IP, add AcceptorInfo to registration, complete failover |
| `src/net/registrar.cpp` | Fix `handle_udp_packet()` to actually send UDP response, store UDP socket for responses |

---

## Testing Considerations

1. **Unit Tests:**
   - `test_registrar_server.cpp`: Test Register/Heartbeat handling
   - `test_registrar_client.cpp`: Test local IP detection, registration format

2. **Integration Tests:**
   - Two-node cluster: verify registration, heartbeat, node discovery
   - Failover: kill server, verify client reconnects or elects new server

3. **Multi-node Test:**
   - Run on multiple machines to verify real IP usage

---

## Dependencies

- `getifaddrs()` / `freeifaddrs()` from `<ifaddrs.h>` and `<sys/types.h>`
- `IFF_UP`, `IFF_RUNNING`, `IFF_LOOPBACK` flags from `<net/if.h>`

---

## Open Questions

1. Should `AcceptorInfo` be configurable per-actor or per-node?
2. How should the server signal `UdpRegistrar` when it becomes the new server during failover?
3. Should there be a maximum retry count for failover before giving up?
