# UDP Registrar and HostResolver Design Specification

## Overview

The UDP Registrar provides **service discovery** for the HPActor distributed actor framework via UDP broadcast/unicast. It enables nodes to discover each other on a local network without centralized coordination, inspired by mDNS/SPDZ conventions.

## Components

### 1. RegistrarConfig

Configuration for the UDP registrar:

```cpp
struct RegistrarConfig {
    uint16_t udp_port = 5353;                     // Default mDNS-style port
    std::chrono::milliseconds heartbeat_interval{5000};
    std::chrono::milliseconds expiration_timeout{15000};
    std::vector<StaticRouteConfig> static_routes;
    bool enable_broadcast = true;
};
```

| Field | Default | Description |
|-------|---------|-------------|
| `udp_port` | 5353 | UDP port for discovery traffic |
| `heartbeat_interval` | 5000ms | How often to broadcast NodeAnnounce |
| `expiration_timeout` | 15000ms | Time before a node is considered offline |
| `static_routes` | empty | Pre-configured static node routes |
| `enable_broadcast` | true | Whether to send broadcast announcements |

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

1. Create UDP socket with SO_BROADCAST
2. Bind to configured UDP port
3. Add static routes to registry
4. Send initial NodeAnnounce broadcast
5. Start periodic broadcast timer (every heartbeat_interval)

### Node Discovery Flow

1. **Broadcast Announce**: Every heartbeat_interval, broadcast NodeAnnounce to 255.255.255.255
2. **Receive Announce**: When receiving announce from new node, add to registry and invoke node_callback(online=true)
3. **Static Route Probe**: Periodically probe static route nodes to verify liveness
4. **Expiration**: Nodes without announcements within expiration_timeout are removed

### Message Handling

**handle_packet():**
1. Validate magic and version
2. Extract sender NodeId (ignore if local)
3. Route to appropriate handler by type

**handle_announce():**
- Update registry with sender info
- Invoke node_callback(online=true) if new node

**handle_leave():**
- Remove node from registry
- Invoke node_callback(online=false)

**handle_probe():**
- Respond with NodeProbeAck

**handle_probe_ack():**
- Remove probe from pending
- Update endpoint last_seen

### Shutdown (stop)

1. Set running_ = false
2. Close UDP socket

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