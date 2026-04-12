# Phase 5: Actor Service Discovery

## Overview

Phase 5 implements service discovery for the HPActor distributed actor framework, enabling nodes to discover each other dynamically via an embedded dual-mode TCP/UDP registrar with automatic failover.

## Goals

- **Dual-Mode Registrar**: TCP-based registration with server/client failover
- **UDP Resolution**: Lightweight unicast resolution queries
- **Static Routes**: Configured node endpoints with hostname support
- **DNS Resolution**: Hostname-to-IP resolution with caching
- **Automatic Failover**: Server election when current server fails
- **Integration**: ActorSystem owns Transport and Registrar

## Architecture

```
ActorSystem
├── UdpRegistrar (dual-mode TCP/UDP)
│   ├── NodeRegistry (owned)
│   ├── HostResolver (owned)
│   ├── TCP socket (server mode) OR TCP connection (client mode)
│   └── UDP socket for resolution queries
└── TcpTransport
    └── ConnectionPool per remote node

ActorProxy::send() → TcpTransport::send() → ConnectionPool → TlsConnection
TcpTransport::connect(NodeId) → Registrar::get_endpoint() → HostResolver::resolve() → TcpTransport::connect(NodeId, ip, port)
```

## Registrar Modes

### Server Mode

When a node starts, the registrar attempts to bind TCP port 5353. If successful, it runs in **server mode**:

- Binds TCP port 5353 for incoming registration connections from client nodes
- Listens on UDP 0.0.0.0:5353 for resolution queries from any host
- Maintains authoritative NodeRegistry for all nodes on the host
- Pushes NodeJoin/NodeLeave events to connected clients via TCP

### Client Mode

If the TCP bind fails (port already taken), the registrar runs in **client mode**:

- Connects via TCP to the server node at localhost:5353
- Sends its own registration over TCP
- Performs resolution queries via UDP unicast to the server's UDP port
- Maintains TCP connection for registration keepalive
- Detects server disconnection and races to become new server

### Failover

When the server node terminates:

1. TCP connections to client nodes close
2. Client nodes detect the disconnection
3. All clients race to bind TCP port 5353
4. First to succeed becomes the new server
5. Others reconnect as clients
6. All nodes re-register their routes with the new server

This failover is automatic and takes milliseconds.

## Registration

When a node starts, it registers with the registrar. Registration includes:

- Node ID (unique within the host)
- Host address (IP or DNS hostname)
- List of acceptors this node is running

For each acceptor:
- Port number
- Handshake protocol version
- Network protocol version
- TLS flag (whether encryption is required)

The TCP connection from client to server stays open. It serves two purposes:
- Maintaining registration (if the connection drops, the node is considered dead)
- Enabling the server to push updates to clients

## Data Structures

### AcceptorInfo

```cpp
struct AcceptorInfo {
    uint16_t port;
    uint8_t handshake_version;
    uint8_t protocol_version;
    bool tls_required;
};
```

### NodeEndpoint

```cpp
struct NodeEndpoint {
    NodeId node_id = 0;
    std::string host;                     // IP or DNS hostname
    uint16_t tcp_port = 0;                // TCP listening port
    bool is_static_route = false;
    std::vector<AcceptorInfo> acceptors;
    std::chrono::steady_clock::time_point last_seen;
};
```

### HostResolver

Hostname-to-IP resolution with caching:

```cpp
class HostResolver {
public:
    // Resolve hostname to IP address (blocking)
    std::string resolve(const std::string& hostname);

    // Async resolution - returns immediately, callback when done
    void resolve_async(const std::string& hostname,
                       std::function<void(std::string ip)> callback);

    // Get cached IP for hostname
    std::string get_cached(const std::string& hostname) const;

    // Cache hostname -> IP mapping with TTL
    void cache(const std::string& hostname, const std::string& ip,
               std::chrono::seconds ttl = std::chrono::seconds(300));

    // Clear expired entries
    void clear_expired();

private:
    struct CacheEntry {
        std::string ip;
        std::chrono::steady_clock::time_point expires_at;
    };

    std::unordered_map<std::string, CacheEntry> cache_;
    mutable std::mutex mutex_;
};
```

**DNS Resolution Strategy**:
1. Check in-memory cache
2. If not found and hostname is IPv4 address string, use `inet_pton()` directly
3. Otherwise call `getaddrinfo()` for DNS resolution
4. Cache result with TTL (default 5 minutes)

## Protocol

### TCP Registration Protocol

Persistent connection from client to server:

| Direction | Message | Payload |
|-----------|---------|---------|
| Client → Server | Register | node_id, host, acceptors[] |
| Client → Server | Heartbeat | (empty) |
| Server → Client | NodeJoin | node_id, host, acceptors[] |
| Server → Client | NodeLeave | node_id |
| Server → Client | Accept | (empty, confirms registration) |
| Server → Client | Error | error_code |

**Registration Flow:**
1. Client connects to server via TCP
2. Client sends Register message with its node info and acceptor list
3. Server responds with Accept (or Error if name taken)
4. Client sends Heartbeat every 5 seconds
5. If server doesn't receive heartbeat for 15 seconds, node is considered dead

### UDP Resolution Protocol

Lightweight unicast queries for resolving node addresses:

```
[SenderNodeId: 4 bytes][TargetNodeId: 4 bytes] = 8 bytes
```

Response format:
```
[HostLength: 1 byte][Host: N bytes][TcpPort: 2 bytes][AcceptorCount: 1 byte]
[Acceptor: port(2) + handshake_ver(1) + protocol_ver(1) + tls(1)]...
```

| Type | Name | Purpose |
|------|------|---------|
| ResolveQuery | Query | Request endpoint info for a target node |
| ResolveResponse | Response | Return endpoint information |

### Static Routes

Static routes are pre-configured nodes that are always known:

```cpp
struct StaticRouteConfig {
    NodeId node_id = 0;
    std::string address;     // IP or DNS hostname
    uint16_t port = 0;
};
```

Static routes are probed periodically (every 30s) to verify connectivity. They do not expire.

## RegistrarConfig

```cpp
struct RegistrarConfig {
    uint16_t udp_port = 5353;                      // UDP resolution port
    uint16_t tcp_port = 5353;                      // TCP registration port
    std::chrono::milliseconds heartbeat_interval{5000};
    std::chrono::milliseconds expiration_timeout{15000};
    std::chrono::milliseconds probe_interval{30000};
    std::vector<StaticRouteConfig> static_routes;
    bool enable_broadcast = false;                  // Not used in TCP mode
    bool disable_server = false;                   // Always run as client
};
```

## File Structure

### Modified Files

| File | Change |
|------|--------|
| `include/hpactor/net/registrar.hpp` | Refactor for dual-mode (TCP server/client), add AcceptorInfo, update NodeEndpoint |
| `src/net/registrar.cpp` | Implement server/client mode, TCP registration, failover |
| `include/hpactor/net/transport.hpp` | Add `connect(NodeId)` overload for registry-based connect |
| `src/net/tcp_transport.cpp` | Wire up registrar for endpoint lookup |

### New/Updated Types

**Removed UDP message types:**
- NodeAnnounce (replaced by TCP Register)
- NodeProbe (not needed with TCP keepalive)
- NodeProbeAck (not needed)

**Simplified UDP message types:**
- NodeQuery / NodeResponse → ResolveQuery / ResolveResponse

## API Changes

### ActorSystem

```cpp
class ActorSystem {
    // Network initialization
    void start_network();
    void stop_network();

    // Accessors
    net::Transport* transport() { return transport_.get(); }
    net::UdpRegistrar* registrar() { return registrar_.get(); }

private:
    std::unique_ptr<net::TcpTransport> transport_;
    std::unique_ptr<net::UdpRegistrar> registrar_;
    std::unique_ptr<net::EventLoop> network_loop_;
    std::thread network_thread_;
};
```

### TcpTransport::connect(NodeId)

```cpp
ConnectionPtr TcpTransport::connect(NodeId remote_node_id) {
    // 1. Lookup endpoint in registry
    NodeEndpoint* ep = registrar_->get_endpoint(remote_node_id);
    if (!ep) return nullptr;

    // 2. Resolve hostname to IP (if needed)
    std::string ip = host_resolver_.resolve(ep->host);

    // 3. Connect to resolved IP:port
    return connect(remote_node_id, ip, ep->tcp_port);
}
```

## Testing

- `test_registrar.cpp` — Unit tests for RegistrarConfig, HostResolver, NodeRegistry, server/client mode, failover

## Out of Scope

- Remote actor spawn (Phase 6)
- Actor migration
- Distributed supervision
- Application discovery (weights, load balancing)
- Centralized configuration
- Event notifications

## Limitations

The embedded registrar is minimal by design:

- **No application discovery** — Discover nodes only, not specific applications
- **No load balancing metadata** — No weight system
- **No centralized configuration** — Each node maintains its own configuration
- **No event notifications** — Pull-based discovery only
- **No topology awareness** — Treats all nodes equally
- **Limited scalability** — UDP query model works for small to medium clusters
