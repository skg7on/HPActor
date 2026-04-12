# Phase 5: Actor Service Discovery

## Overview

Phase 5 implements service discovery for the HPActor distributed actor framework, enabling nodes to discover each other dynamically via UDP broadcast and static route configuration.

## Goals

- **UDP Registrar**: Broadcast-based node discovery on local subnet
- **Static Routes**: Configured node endpoints with hostname support
- **DNS Resolution**: Hostname-to-IP resolution with caching
- **Integration**: ActorSystem owns Transport and Registrar

## Architecture

```
ActorSystem
├── TcpTransport (owned)
│   └── ConnectionPool per remote node
├── UdpRegistrar (owned)
│   ├── NodeRegistry (map of known nodes)
│   ├── HostResolver (DNS cache)
│   └── UDP socket for broadcast/unicast
└── EventLoop (network I/O)

ActorProxy::send() → TcpTransport::send() → ConnectionPool → TlsConnection
```

## Key Components

### NodeEndpoint

```cpp
struct NodeEndpoint {
    NodeId node_id = 0;
    std::string host;        // IP or DNS hostname
    uint16_t tcp_port = 0; // TCP listening port
    std::chrono::steady_clock::time_point last_seen;
    bool is_static_route = false;
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
};
```

**DNS Resolution Strategy**:
1. Check in-memory cache
2. If not found and hostname is IPv4 address string, use `inet_pton()` directly
3. Otherwise call `getaddrinfo()` for DNS resolution
4. Cache result with TTL (default 5 minutes)

### UDP Registrar Protocol

All UDP messages have 12-byte header + payload:

```
[Magic: 4 bytes = 0x48504143 "HPAC"]
[Version: 1 byte = 0x01]
[Type: 1 byte]
[NodeId: 4 bytes]
[Payload: N bytes]
```

| Type | Name | Payload |
|------|------|---------|
| 0x01 | NodeAnnounce | tcp_port, actor_count |
| 0x02 | NodeQuery | target_node_id |
| 0x03 | NodeResponse | tcp_port |
| 0x04 | NodeLeave | (empty) |
| 0x05 | NodeProbe | probe_id, timestamp |
| 0x06 | NodeProbeAck | probe_id, timestamp |

### Discovery Flow

1. **Startup**: Node reads static routes (with hostnames), starts UDP listener, broadcasts `NodeAnnounce`
2. **Dynamic Discovery**: Broadcast `NodeAnnounce` every 5s; on receipt, update registry
3. **Static Route Probe**: Unicast `NodeProbe` every 30s to static routes
4. **Expiration**: Nodes not heard within 15s are removed
5. **DNS Resolution**: When connecting, resolve hostname via `HostResolver` if needed

## File Structure

### New Files

| File | Purpose |
|------|---------|
| `include/hpactor/net/registrar.hpp` | UdpRegistrar, NodeRegistry, HostResolver, message types |
| `src/net/registrar.cpp` | UDP socket, broadcast/unicast, expiration, DNS resolution |
| `tests/net/test_registrar.cpp` | Unit tests |

### Modified Files

| File | Change |
|------|--------|
| `include/hpactor/actor_system.hpp` | Add `transport_`, `registrar_` members |
| `src/actor/actor_system.cpp` | Initialize network components on startup |
| `include/hpactor/net/transport.hpp` | Add `connect(NodeId)` overload |
| `src/ref/actor_proxy.cpp` | Implement working `send()` via transport |

## API Changes

### RegistrarConfig

```cpp
struct RegistrarConfig {
    uint16_t udp_port = 5353;                     // Default mDNS-style port
    std::chrono::milliseconds heartbeat_interval{5000};
    std::chrono::milliseconds expiration_timeout{15000};
    std::vector<StaticRouteConfig> static_routes;
    bool enable_broadcast = true;
};

struct StaticRouteConfig {
    NodeId node_id = 0;
    std::string address;     // IP or DNS hostname
    uint16_t port = 0;
};
```

### ActorSystem

```cpp
class ActorSystem {
    net::Transport* transport() { return transport_.get(); }
    UdpRegistrar* registrar() { return registrar_.get(); }

private:
    std::unique_ptr<net::TcpTransport> transport_;
    std::unique_ptr<UdpRegistrar> registrar_;
    std::unique_ptr<net::EventLoop> network_loop_;
    std::thread network_thread_;
};
```

### ActorProxy::send()

```cpp
void ActorProxy::send(const ActorAddress& target, MessageVariant msg) {
    // 1. Serialize message
    bytes payload = serializer.encode(tag, msg);
    // 2. Create frame
    net::Frame frame{address_, target, payload, MessageId::generate().value()};
    // 3. Send via transport
    transport_->send(target, frame.encode());
}
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

- `test_registrar.cpp` — Unit tests for RegistrarConfig, HostResolver, NodeRegistry

## Out of Scope

- Remote actor spawn (Phase 6)
- Actor migration
- Distributed supervision
