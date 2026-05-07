# UDP Registrar and HostResolver Design Specification

## Overview

The UDP Registrar provides **service discovery** for the HPActor distributed actor framework via UDP broadcast/unicast. It enables nodes to discover each other on a local network without centralized coordination, inspired by mDNS/SPDZ conventions.

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│ UdpRegistrar (mode-switching facade)                    │
│                                                         │
│ start() probes TCP port → server or client mode         │
│                                                         │
│ ┌─ Server mode ──────────────────────────────────────┐ │
│ │ RegistrarServer (TCP: config_.tcp_port)             │ │
│ │   ├── Acceptor — TCP listener                      │ │
│ │   ├── NodeRegistry — authoritative copy             │ │
│ │   └── RegistrarConnection × N — per-client conns   │ │
│ │                                                     │ │
│ │ UDP socket (UdpRegistrar-owned, config_.udp_port)   │ │
│ │   └── set_read_handler → handle_udp_read_ready()   │ │
│ └─────────────────────────────────────────────────────┘ │
│                                                         │
│ ┌─ Client mode ──────────────────────────────────────┐ │
│ │ RegistrarClient (TCP → server)                      │ │
│ │   ├── RegistrarConnection × 1 — to server          │ │
│ │   └── failover_callback → failover() after 5 retries│ │
│ │                                                     │ │
│ │ NodeRegistry (client_registry_) — seeded from       │ │
│ │   static_routes, updated via NodeJoin/NodeLeave     │ │
│ │                                                     │ │
│ │ UDP socket (UdpRegistrar-owned, config_.udp_port)   │ │
│ │   └── set_read_handler → handle_udp_read_ready()   │ │
│ └─────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────┘
```

**UDP socket ownership**: UdpRegistrar owns the single UDP socket in both modes.
It creates, binds, and registers the read handler via `setup_udp_socket()`. RegistrarServer
does NOT create a UDP socket — it is purely a TCP registration authority.

**EventLoop integration**: When an EventLoop is available, `start()` uses the async
path (`start_server_mode_async()` / `start_client_mode_async()`) which registers
`handle_udp_read_ready()` as the read handler. The EventLoop invokes it on
edge-triggered readability; it performs a non-blocking `recvfrom()` loop and
routes packets to `handle_udp_packet()`.

**Failover**: In client mode, `RegistrarClient` tracks consecutive reconnect
attempts. After 5 failures, it invokes `failover()`, which probes the TCP port
and promotes the node to server mode if the port is free.

## Components

### 1. RegistrarConfig

Configuration for the UDP registrar:

```cpp
struct RegistrarConfig {
    uint16_t udp_port = 5353;
    uint16_t tcp_port = 5353;
    std::chrono::milliseconds heartbeat_interval{5000};
    std::chrono::milliseconds expiration_timeout{15000};
    std::chrono::milliseconds probe_interval{30000};
    std::vector<StaticRouteConfig> static_routes;
    bool disable_server = false;
};
```

| Field | Default | Description |
|-------|---------|-------------|
| `udp_port` | 5353 | UDP port for discovery traffic |
| `tcp_port` | 5353 | TCP port for registration server |
| `heartbeat_interval` | 5000ms | How often to send heartbeat |
| `expiration_timeout` | 15000ms | Time before a node is considered offline |
| `probe_interval` | 30000ms | Interval for static route liveness probes |
| `static_routes` | empty | Pre-configured static node routes |
| `disable_server` | false | Force client mode (never become server) |

### 2. StaticRouteConfig

Pre-configured route to a known node:

```cpp
struct StaticRouteConfig {
    NodeId node_id = 0;
    std::string address;     // IP or DNS hostname
    uint16_t port = 0;
};
```

### 3. NodeEndpoint

Information about a discovered node:

```cpp
struct NodeEndpoint {
    NodeId node_id = 0;
    std::string host;        // IP or DNS hostname
    uint16_t tcp_port = 0;  // TCP listening port
    std::chrono::steady_clock::time_point last_seen;
    bool is_static_route = false;
};
```

### 4. HostResolver

Hostname to IP resolution with caching.

**Key operations:**

- `resolve(hostname)` - Blocking DNS resolution with cache lookup
- `resolve_async(hostname, callback)` - Async resolution
- `get_cached(hostname)` - Get cached IP (empty if not cached)
- `cache(hostname, ip, ttl)` - Cache a hostname -> IP mapping
- `clear_expired()` - Remove expired cache entries

**Resolution algorithm:**
1. Check cache for valid (non-expired) entry
2. If hostname is already an IP address (inet_pton succeeds), cache and return
3. Call getaddrinfo() for DNS resolution
4. Cache result with TTL (default 300s)

### 5. NodeRegistry

Registry of known nodes (NodeId -> NodeEndpoint).

**Key operations:**
- `upsert_endpoint(endpoint)` - Add or update an endpoint
- `remove_endpoint(node_id)` - Remove an endpoint
- `get(node_id)` - Get endpoint (nullptr if not found)
- `has(node_id)` - Check if endpoint exists
- `all()` - Get all endpoints
- `remove_expired()` - Remove expired non-static endpoints

### 6. UdpRegistrar

UDP-based node discovery with protocol support.

## Protocol Specification

### Message Format

All messages use a 12-byte header followed by payload:

```
+-------------+--------+--------+----------------+
| Magic (4B)  | Ver(1B)| Type(1B)| NodeId (4B)   |
+-------------+--------+--------+----------------+
```

| Field | Size | Value |
|-------|------|-------|
| Magic | 4B | 0x48504143 ("HPAC") - big-endian |
| Version | 1B | 0x01 |
| Type | 1B | Message type enum |
| NodeId | 4B | Sender's node ID - big-endian |

### Message Types

| Type | Value | Description |
|------|-------|-------------|
| NodeAnnounce | 0x01 | Periodic broadcast announcement |
| NodeQuery | 0x02 | Query for specific node |
| NodeResponse | 0x03 | Response to NodeQuery |
| NodeLeave | 0x04 | Node is going offline |
| NodeProbe | 0x05 | Liveness probe |
| NodeProbeAck | 0x06 | Probe acknowledgment |

### Payload Structures

**NodeAnnouncePayload:**
```cpp
struct NodeAnnouncePayload {
    uint16_t tcp_port;      // TCP listening port
    uint16_t actor_count;   // Number of actors on node
};
```

**NodeQueryPayload:**
```cpp
struct NodeQueryPayload {
    NodeId target_node_id; // Node being queried
};
```

**NodeResponsePayload:**
```cpp
struct NodeResponsePayload {
    uint16_t tcp_port;      // Responding node's TCP port
};
```

**NodeProbePayload:**
```cpp
struct NodeProbePayload {
    uint64_t probe_id;      // Unique probe identifier
    uint64_t timestamp;     // Probe timestamp (epoch ms)
};
```

## Behavior

### Startup (start)

1. Probe TCP port via `bind()` to determine server vs. client mode
2. **When EventLoop is available**: use async path (`start_server_mode_async` / `start_client_mode_async`)
3. **Without EventLoop**: use sync path (`start_server_mode` / `start_client_mode`)
4. Both paths call `setup_udp_socket()` which creates, binds, and registers the read handler
5. Add static routes to registry (client mode)
6. Connect to server and send registration (client mode)

### Node Discovery Flow

1. **TCP Registration**: Clients connect to the server, send `Register` with endpoint info
2. **Heartbeat**: Clients send periodic `Heartbeat` messages via TCP to maintain presence
3. **Broadcast**: Server broadcasts `NodeJoin`/`NodeLeave` to all connected clients
4. **UDP Resolution**: `ResolveQuery` is handled via the UDP socket read handler; server looks up the endpoint and sends `ResolveResponse`
5. **Expiration**: Nodes without heartbeats within `expiration_timeout` are removed

### Message Handling

**handle_udp_read_ready():**
1. Called by EventLoop when UDP socket is readable (edge-triggered)
2. Non-blocking `recvfrom()` loop until EAGAIN
3. Routes to `handle_udp_packet()` for parsing and dispatch

**handle_udp_packet():**
1. Validates magic and version
2. Routes to `ResolveQuery` or `ResolveResponse` handler

### Failover

In client mode, `RegistrarClient` tracks consecutive reconnect failures. After 5
consecutive disconnects without a successful reconnection, it invokes
`failover()`, which re-probes the TCP port — if now free, the node promotes
itself to server mode.

### Shutdown (stop)

1. Stop server or client
2. Clear EventLoop read handler and fd registration
3. Close UDP socket

## Thread Safety

- All classes use mutexes for internal state protection
- HostResolver: caches and pending async resolutions
- NodeRegistry: endpoints map
- UdpRegistrar: pending_probes map

## Platform Considerations

### macOS

- Uses kqueue() via EventLoop
- be64toh replaced with portable swap_be64() implementation

### Linux

- Uses epoll() via EventLoop
- be64toh available in glibc

## Files

| File | Purpose |
|------|---------|
| `include/hpactor/net/registrar.hpp` | Public header with all types |
| `src/net/registrar.cpp` | Implementation |
| `tests/net/test_registrar.cpp` | Unit tests |

## Integration

The UdpRegistrar integrates with:
- **EventLoop** - for socket I/O and periodic timers
- **ActorSystem** - for local node ID and TCP port configuration
- **ActorProxy** - for resolving NodeId to transport connections