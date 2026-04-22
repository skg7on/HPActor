# Communication Endpoint Abstraction - Design Spec

**Date:** 2026-04-23
**Author:** SKG7ON
**Status:** Draft

## Overview

Replace the string-based `NodeId` type with a type-safe `CommunicationEndpoint` abstraction that supports both IPv4 and IPv6. This provides protocol-aware address representation with zero heap allocation.

## Background

Current `NodeId = std::string` using "host:port" format has issues:
- No type safety — any string is a valid NodeId
- No protocol awareness — IPv4 vs IPv6 not distinguished
- Manual parsing required for address extraction
- No compile-time verification of format correctness

## Design Decisions

| Decision | Choice |
|----------|--------|
| Endpoint type | `std::variant<Ipv4Endpoint, Ipv6Endpoint>` |
| Protocol enum | Separate `enum class Protocol { IPv4, IPv6 }` |
| Address storage | Network byte order (big-endian) |
| IPv4 storage | `uint32_t addr` (4 bytes) |
| IPv6 storage | `std::array<uint8_t, 16>` (16 bytes) |
| Port storage | `uint16_t port` (network byte order) |
| ActorAddress | Holds `CommunicationEndpoint` directly |
| TcpTransport | Direct connect, no registry lookup |
| Serialization | New binary format, no backward compat |
| Testing | Comprehensive equality/hashing tests |

## Endpoint Types

```cpp
enum class Protocol { IPv4, IPv6 };

struct Ipv4Endpoint {
    uint32_t addr;   // Network byte order (big-endian)
    uint16_t port;   // Network byte order

    constexpr Ipv4Endpoint(uint32_t a, uint16_t p) noexcept : addr(a), port(p) {}
};

struct Ipv6Endpoint {
    std::array<uint8_t, 16> addr;  // Network byte order
    uint16_t port;                  // Network byte order

    constexpr Ipv6Endpoint() noexcept : addr{}, port(0) {}
    constexpr Ipv6Endpoint(std::array<uint8_t, 16> a, uint16_t p) noexcept : addr(a), port(p) {}
};

using CommunicationEndpoint = std::variant<Ipv4Endpoint, Ipv6Endpoint>;
```

## CommunicationEndpoint Operations

```cpp
class CommunicationEndpoint {
public:
    // Protocol access
    Protocol protocol() const;
    int address_family() const;  // AF_INET or AF_INET6

    // Address conversion for socket operations
    sockaddr* to_sockaddr();
    socklen_t sockaddr_length() const;

    // String representation
    std::string to_string() const;  // "192.168.1.1:5353" or "[::1]:5353"

    // Network classification
    bool is_loopback() const;
    bool is_private_network() const;
    bool is_unspecified() const;

    // Observers
    uint16_t port() const;
    bool is_ipv4() const;
    bool is_ipv6() const;
};
```

## ActorAddress Changes

```cpp
struct ActorAddress {
    CommunicationEndpoint endpoint;  // replaces NodeId
    ActorType type = 0;
    ActorId id;
    uint64_t incarnation = 0;

    bool is_local() const noexcept;
    std::string to_string() const;  // delegates to endpoint.to_string()
};

const ActorAddr invalid_actor_addr{};
```

## TcpTransport Changes

TcpTransport connects directly using `CommunicationEndpoint`:
- Constructor takes `CommunicationEndpoint local_endpoint`
- `connect(remote_endpoint)` establishes TCP/TLS connection
- No registry lookup required — caller provides endpoint

## Serialization Format

**Wire protocol:**
- IPv4: `[0x04][addr: 4][port: 2]` = 7 bytes
- IPv6: `[0x06][addr: 16][port: 2]` = 19 bytes

Protocol byte: `0x04` = IPv4, `0x06` = IPv6

No length prefix needed — protocol byte determines format and total size.

## std::hash Specializations

Both `Ipv4Endpoint` and `Ipv6Endpoint` require `std::hash` specializations. `CommunicationEndpoint` delegates to the active variant member.

## Files to Modify

### Headers
- `include/hpactor/types/types.hpp` — remove `NodeId`, add endpoint types and helpers
- `include/hpactor/ref/actor_address.hpp` — `NodeId` → `CommunicationEndpoint`
- `include/hpactor/net/transport.hpp` — update Connection and Transport interfaces
- `include/hpactor/net/tcp_transport.hpp` — endpoint-based constructor and methods
- `include/hpactor/net/connection_pool.hpp` — endpoint-based connection
- `include/hpactor/net/registrar.hpp` — NodeRegistry stores endpoints

### Implementation
- `src/net/frame.cpp` — new binary encoding/decoding
- `src/net/tcp_transport.cpp` — direct endpoint connect
- `src/net/connection_pool.cpp` — endpoint to socket address
- `src/net/registrar.cpp` — endpoint storage
- `src/net/registrar_server.cpp` — endpoint encoding
- `src/net/registrar_client.cpp` — endpoint encoding
- `src/net/acceptor.cpp` — endpoint construction

### Tests
- `tests/net/test_communication_endpoint.cpp` — unit tests for endpoint
- `tests/net/test_actor_address.cpp` — address with endpoint
- `tests/net/test_tcp_transport.cpp` — transport with endpoints

## Testing Plan

1. **Endpoint construction** — IPv4/IPv6 endpoints with correct byte order
2. **Equality** — same IP+port = equal; IPv4 vs IPv6 = not equal
3. **Hashing** — equal endpoints produce equal hashes
4. **Serialization round-trip** — encode then decode yields original endpoint
5. **String format** — "192.168.1.1:5353" for IPv4, "[::1]:5353" for IPv6
6. **ActorAddress** — construction with endpoint, to_string delegation
7. **Network classification** — is_loopback, is_private_network correctness

## Removed Functions

- `node_id_host(NodeId)` — replaced by endpoint accessor
- `node_id_port(NodeId)` — replaced by endpoint.port()
- `make_node_id(host, port)` — replaced by `Ipv4Endpoint(addr, port)` or `Ipv6Endpoint(addr, port)`
- `is_local_node_id(NodeId)` — replaced by endpoint.is_loopback() or explicit local interface check

## Constraints

- C++20, no exceptions, no RTTI
- All endpoint operations constexpr where possible
- No heap allocation in endpoint types
- Serialization must handle both protocols uniformly