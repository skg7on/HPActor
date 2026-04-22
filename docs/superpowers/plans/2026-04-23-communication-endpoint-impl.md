# Communication Endpoint Abstraction - Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace string-based NodeId with type-safe CommunicationEndpoint abstraction supporting IPv4/IPv6 with network byte order storage.

**Architecture:** Two concrete endpoint structs (Ipv4Endpoint, Ipv6Endpoint) wrapped in std::variant<>. Protocol-agnostic operations via std::visit. Binary serialization with protocol byte prefix. TcpTransport connects directly using endpoint without registry lookup.

**Tech Stack:** C++20, no exceptions (-fno-exceptions), no RTTI (-fno-rtti), network byte order storage.

---

## File Structure

| Layer | Files | Responsibility |
|-------|-------|----------------|
| Types | `include/hpactor/types/types.hpp` | Endpoint types, Protocol enum, std::hash |
| Ref | `include/hpactor/ref/actor_address.hpp` | ActorAddress with endpoint field |
| Transport | `include/hpactor/net/transport.hpp`, `tcp_transport.hpp`, `connection_pool.hpp` | Transport interface with endpoint |
| Registrar | `include/hpactor/net/registrar.hpp` | NodeRegistry stores endpoints |
| Frame | `include/hpactor/net/frame.hpp`, `src/net/frame.cpp` | Binary serialization |
| Endpoint impl | `src/net/endpoint.cpp` | endpoint_ops implementation |
| Tests | `tests/net/test_communication_endpoint.cpp`, `test_actor_address.cpp` | Unit tests |

---

## Task 1: Define Endpoint Types in types.hpp

**Files:**
- Modify: `include/hpactor/types/types.hpp:51-101`
- Create: `src/net/endpoint.cpp`
- Create: `tests/net/test_communication_endpoint.cpp`
- Modify: `CMakeLists.txt` (add endpoint.cpp to sources)

**Note:** Remove existing helper functions (`node_id_host`, `node_id_port`, `make_node_id`, `is_local_node_id`) - they are replaced by endpoint operations.

- [ ] **Step 1: Write test for endpoint types**

Create `tests/net/test_communication_endpoint.cpp`:
```cpp
#include <hpactor/types/types.hpp>
#include <cassert>
#include <array>

int main() {
    using namespace hpactor;

    // Test IPv4Endpoint construction
    Ipv4Endpoint ipv4{0x01010101, 5353};  // 1.1.1.1 in network order
    assert(ipv4.port() == 5353);
    assert(ipv4.is_ipv4() == true);
    assert(ipv4.is_ipv6() == false);

    // Test IPv6Endpoint construction
    std::array<uint8_t, 16> loopback_arr{};
    loopback_arr[15] = 1;  // ::1
    Ipv6Endpoint ipv6{loopback_arr, 8080};
    assert(ipv6.port() == 8080);
    assert(ipv6.is_ipv6() == true);
    assert(ipv6.is_ipv4() == false);

    // Test CommunicationEndpoint variant
    CommunicationEndpoint ep = ipv4;
    assert(std::holds_alternative<Ipv4Endpoint>(ep));
    assert(!std::holds_alternative<Ipv6Endpoint>(ep));

    // Test is_loopback
    Ipv4Endpoint loopback4{0x7F000001, 1234};  // 127.0.0.1
    assert(loopback4.is_loopback() == true);
    assert(Ipv4Endpoint{0xC0A80001, 1234}.is_loopback() == false);  // 192.168.0.1

    std::array<uint8_t, 16> loopback6_arr{};
    loopback6_arr[15] = 1;
    assert(Ipv6Endpoint{loopback6_arr, 1234}.is_loopback() == true);

    // Test is_private_network
    assert(Ipv4Endpoint{0x0A000001, 1234}.is_private_network() == true);   // 10.0.0.1
    assert(Ipv4Endpoint{0xAC100001, 1234}.is_private_network() == true);   // 172.16.0.1
    assert(Ipv4Endpoint{0xC0A80001, 1234}.is_private_network() == true);  // 192.168.0.1
    assert(Ipv4Endpoint{0xC0A80001, 1234}.is_private_network() == true);
    assert(Ipv4Endpoint{0x08080808, 1234}.is_private_network() == false); // 8.8.8.8

    // Test sockaddr conversion
    sockaddr_in addr4;
    ipv4.to_sockaddr(&addr4);
    assert(addr4.sin_family == AF_INET);
    assert(addr4.sin_port == 5353);
    assert(addr4.sin_addr.s_addr == 0x01010101);

    sockaddr_in6 addr6;
    ipv6.to_sockaddr(&addr6);
    assert(addr6.sin6_family == AF_INET6);
    assert(addr6.sin6_port == 8080);

    // Test hash equality
    Ipv4Endpoint ipv4_copy{0x01010101, 5353};
    assert(std::hash<Ipv4Endpoint>{}(ipv4) == std::hash<Ipv4Endpoint>{}(ipv4_copy));

    return 0;
}
```

Run: `ninja -C build && ./build/tests/net/test_communication_endpoint`
Expected: Compiles and runs

- [ ] **Step 2: Remove NodeId and add Protocol enum**

Replace lines 51-101 in types.hpp with endpoint types. Important: The includes at the top need `<array>` and `<cstring>` added.

```cpp
// -----------------------------------------------------------------------------
// Protocol - network protocol family
// -----------------------------------------------------------------------------
enum class Protocol { IPv4, IPv6 };

// -----------------------------------------------------------------------------
// Ipv4Endpoint - IPv4 address and port (network byte order)
// -----------------------------------------------------------------------------
struct Ipv4Endpoint {
    uint32_t addr;   // Network byte order (big-endian)
    uint16_t port;   // Network byte order

    constexpr Ipv4Endpoint() noexcept : addr(0), port(0) {}
    constexpr Ipv4Endpoint(uint32_t a, uint16_t p) noexcept : addr(a), port(p) {}

    [[nodiscard]] constexpr uint16_t port() const noexcept { return port; }
    [[nodiscard]] constexpr bool is_ipv4() const noexcept { return true; }
    [[nodiscard]] constexpr bool is_ipv6() const noexcept { return false; }
    [[nodiscard]] constexpr bool is_loopback() const noexcept;
    [[nodiscard]] constexpr bool is_private_network() const noexcept;
    [[nodiscard]] constexpr bool is_unspecified() const noexcept;

    // For socket operations
    constexpr socklen_t sockaddr_length() const noexcept { return sizeof(sockaddr_in); }
    void to_sockaddr(sockaddr_in* out) const noexcept;
};

[[nodiscard]] constexpr bool Ipv4Endpoint::is_loopback() const noexcept {
    return (addr & 0xFF000000) == 0x7F000000;
}

[[nodiscard]] constexpr bool Ipv4Endpoint::is_private_network() const noexcept {
    uint8_t b1 = (addr >> 24) & 0xFF;
    uint32_t rest = addr & 0xFFFF0000;
    return b1 == 10 ||
           (b1 == 172 && (rest & 0xFFF00000) == 0x10A00000) ||
           (b1 == 192 && rest == 0xC0A80000);
}

[[nodiscard]] constexpr bool Ipv4Endpoint::is_unspecified() const noexcept {
    return addr == 0;
}

inline void Ipv4Endpoint::to_sockaddr(sockaddr_in* out) const noexcept {
    out->sin_family = AF_INET;
    out->sin_port = port;
    out->sin_addr.s_addr = addr;
    std::memset(out->sin_zero, 0, sizeof(out->sin_zero));
}

// -----------------------------------------------------------------------------
// Ipv6Endpoint - IPv6 address and port (network byte order)
// -----------------------------------------------------------------------------
struct Ipv6Endpoint {
    std::array<uint8_t, 16> addr;  // Network byte order
    uint16_t port;                 // Network byte order

    constexpr Ipv6Endpoint() noexcept : addr{}, port(0) {}
    constexpr Ipv6Endpoint(std::array<uint8_t, 16> a, uint16_t p) noexcept
        : addr(a), port(p) {}

    [[nodiscard]] constexpr uint16_t port() const noexcept { return port; }
    [[nodiscard]] constexpr bool is_ipv4() const noexcept { return false; }
    [[nodiscard]] constexpr bool is_ipv6() const noexcept { return true; }
    [[nodiscard]] bool is_loopback() const noexcept;
    [[nodiscard]] bool is_private_network() const noexcept;
    [[nodiscard]] bool is_unspecified() const noexcept;

    // For socket operations
    constexpr socklen_t sockaddr_length() const noexcept { return sizeof(sockaddr_in6); }
    void to_sockaddr(sockaddr_in6* out) const noexcept;
};

inline bool Ipv6Endpoint::is_loopback() const noexcept {
    for (int i = 0; i < 15; ++i) if (addr[i] != 0) return false;
    return addr[15] == 1;
}

inline bool Ipv6Endpoint::is_private_network() const noexcept {
    return (addr[0] & 0xFE) == 0xFC;
}

inline bool Ipv6Endpoint::is_unspecified() const noexcept {
    for (int i = 0; i < 16; ++i) if (addr[i] != 0) return false;
    return true;
}

inline void Ipv6Endpoint::to_sockaddr(sockaddr_in6* out) const noexcept {
    out->sin6_family = AF_INET6;
    out->sin6_port = port;
    std::memcpy(out->sin6_addr.s6_addr, addr.data(), 16);
    out->sin6_flowinfo = 0;
    out->sin6_scope_id = 0;
}

// -----------------------------------------------------------------------------
// CommunicationEndpoint - variant over IPv4/IPv6
// -----------------------------------------------------------------------------
using CommunicationEndpoint = std::variant<Ipv4Endpoint, Ipv6Endpoint>;

// to_sockaddr free function for variant
inline void to_sockaddr(const CommunicationEndpoint& ep, sockaddr* out, socklen_t* len) {
    if (auto* ipv4 = std::get_if<Ipv4Endpoint>(&ep)) {
        if (*len >= sizeof(sockaddr_in)) {
            ipv4->to_sockaddr(reinterpret_cast<sockaddr_in*>(out));
            *len = sizeof(sockaddr_in);
        }
    } else if (auto* ipv6 = std::get_if<Ipv6Endpoint>(&ep)) {
        if (*len >= sizeof(sockaddr_in6)) {
            ipv6->to_sockaddr(reinterpret_cast<sockaddr_in6*>(out));
            *len = sizeof(sockaddr_in6);
        }
    }
}

// -----------------------------------------------------------------------------
// CommunicationEndpoint operations (implemented in cpp)
// -----------------------------------------------------------------------------
namespace endpoint_ops {
    [[nodiscard]] Protocol protocol(const CommunicationEndpoint& ep);
    [[nodiscard]] int address_family(const CommunicationEndpoint& ep);
    [[nodiscard]] std::string to_string(const CommunicationEndpoint& ep);
}
```

Add needed includes to top of types.hpp:
```cpp
#include <array>
#include <cstring>
#include <netinet/in.h>  // for sockaddr_in, sockaddr_in6, AF_INET, AF_INET6
```

Also need to add `<variant>` which is already present.

- [ ] **Step 3: Add std::hash specializations**

Add after CommunicationEndpoint definition:
```cpp
// -----------------------------------------------------------------------------
// std::hash specializations
// -----------------------------------------------------------------------------
template <> struct std::hash<hpactor::Ipv4Endpoint> {
    std::size_t operator()(const hpactor::Ipv4Endpoint& ep) const noexcept {
        std::size_t h = std::hash<uint32_t>{}(ep.addr);
        h ^= std::hash<uint16_t>{}(ep.port) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};

template <> struct std::hash<hpactor::Ipv6Endpoint> {
    std::size_t operator()(const hpactor::Ipv6Endpoint& ep) const noexcept {
        std::size_t h = 0;
        for (uint8_t b : ep.addr) {
            h ^= std::hash<uint8_t>{}(b) + 0x9e3779b9 + (h << 6) + (h >> 2);
        }
        h ^= std::hash<uint16_t>{}(ep.port) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};

template <> struct std::hash<hpactor::CommunicationEndpoint> {
    std::size_t operator()(const hpactor::CommunicationEndpoint& ep) const noexcept {
        if (auto* ipv4 = std::get_if<hpactor::Ipv4Endpoint>(&ep)) {
            return std::hash<hpactor::Ipv4Endpoint>{}(*ipv4);
        }
        return std::hash<hpactor::Ipv6Endpoint>{}(std::get<hpactor::Ipv6Endpoint>(ep));
    }
};
```

- [ ] **Step 4: Implement endpoint_ops functions**

Create `src/net/endpoint.cpp`:
```cpp
#include <hpactor/types/types.hpp>
#include <arpa/inet.h>
#include <cstdio>

namespace hpactor {
namespace endpoint_ops {

Protocol protocol(const CommunicationEndpoint& ep) {
    if (std::holds_alternative<Ipv4Endpoint>(ep)) return Protocol::IPv4;
    return Protocol::IPv6;
}

int address_family(const CommunicationEndpoint& ep) {
    return std::holds_alternative<Ipv4Endpoint>(ep) ? AF_INET : AF_INET6;
}

std::string to_string(const CommunicationEndpoint& ep) {
    char buf[64];
    if (auto* ipv4 = std::get_if<Ipv4Endpoint>(&ep)) {
        struct in_addr addr;
        addr.s_addr = ipv4->addr;
        snprintf(buf, sizeof(buf), "%s:%u", inet_ntoa(addr), ipv4->port());
        return std::string(buf);
    }
    if (auto* ipv6 = std::get_if<Ipv6Endpoint>(&ep)) {
        struct in6_addr addr;
        std::memcpy(addr.s6_addr, ipv6->addr.data(), 16);
        snprintf(buf, sizeof(buf), "[%s]:%u", inet_ntop(AF_INET6, &addr, buf, 40), ipv6->port());
        return std::string(buf);
    }
    return "<invalid>";
}

} // namespace endpoint_ops
} // namespace hpactor
```

- [ ] **Step 5: Add to CMakeLists.txt**

Find the `add_library(hpactor_lib OBJECT` line and add `net/endpoint.cpp` to the list.

- [ ] **Step 6: Run tests and fix**

Run: `ninja -C build && ./build/tests/net/test_communication_endpoint`
Expected: PASS

- [ ] **Step 7: Commit**

```bash
git add include/hpactor/types/types.hpp src/net/endpoint.cpp CMakeLists.txt tests/net/test_communication_endpoint.cpp
git commit -m "feat(types): add CommunicationEndpoint with IPv4/IPv6 support"
```

---

## Task 2: Update ActorAddress to use CommunicationEndpoint

**Files:**
- Modify: `include/hpactor/ref/actor_address.hpp`
- Modify: `src/net/endpoint.cpp` (add is_local implementation)

- [ ] **Step 1: Write test for ActorAddress with endpoint**

Create `tests/net/test_actor_address.cpp`:
```cpp
#include <hpactor/ref/actor_address.hpp>
#include <cassert>

int main() {
    using namespace hpactor;

    // Create IPv4 endpoint
    Ipv4Endpoint ipv4{0x01010101, 5353};
    CommunicationEndpoint ep = ipv4;

    // Create ActorAddress with endpoint
    ActorAddress addr{ep, 1, ActorId(42), 1};

    // Test equality
    ActorAddress addr2{ep, 1, ActorId(42), 1};
    assert(addr == addr2);
    assert(!(addr != addr2));

    // Test to_string delegates to endpoint
    std::string str = addr.to_string();
    assert(str.find("5353") != std::string::npos);

    // Test is_local with loopback
    Ipv4Endpoint loopback{0x7F000001, 8080};  // 127.0.0.1
    ActorAddress local_addr{loopback, 1, ActorId(1), 0};
    assert(local_addr.is_local() == true);

    // Test is_local with non-loopback
    ActorAddress remote_addr{ipv4, 1, ActorId(2), 0};
    assert(remote_addr.is_local() == false);

    return 0;
}
```

Run: `ninja -C build && ./build/tests/net/test_actor_address`
Expected: Fails (missing implementation)

- [ ] **Step 2: Update ActorAddress struct**

Replace `actor_address.hpp` content:
```cpp
#pragma once

#include <functional>
#include <hpactor/types/types.hpp>

namespace hpactor {

// -----------------------------------------------------------------------------
// ActorAddress - unique identifier for an actor across the distributed system
// -----------------------------------------------------------------------------
struct ActorAddress {
    CommunicationEndpoint endpoint;  // replaces NodeId
    ActorType type = 0;
    ActorId id;
    uint64_t incarnation = 0;

    ActorAddress() = default;
    ActorAddress(CommunicationEndpoint ep, ActorType t, ActorId i, uint64_t inc)
        : endpoint(std::move(ep)), type(t), id(i), incarnation(inc) {}

    bool operator==(const ActorAddress& other) const noexcept {
        return endpoint == other.endpoint && type == other.type &&
               id == other.id && incarnation == other.incarnation;
    }
    bool operator!=(const ActorAddress& other) const noexcept {
        return !(*this == other);
    }

    bool is_local() const noexcept {
        if (auto* ipv4 = std::get_if<Ipv4Endpoint>(&endpoint)) {
            return ipv4->is_loopback();
        }
        if (auto* ipv6 = std::get_if<Ipv6Endpoint>(&endpoint)) {
            return ipv6->is_loopback();
        }
        return false;
    }

    explicit operator bool() const {
        return id.value() != 0;
    }

    std::string to_string() const {
        return endpoint_ops::to_string(endpoint);
    }

  private:
    static void hash_combine(size_t& seed, size_t value) noexcept {
        seed ^= value + 0x9e3779b9 + (seed << 6) + (seed >> 2);
    }
};

using ActorAddr = ActorAddress;
inline const ActorAddr invalid_actor_addr{};

} // namespace hpactor

// -----------------------------------------------------------------------------
// std::hash specialization for ActorAddress
// -----------------------------------------------------------------------------
template <> struct std::hash<hpactor::ActorAddress> {
    std::size_t operator()(const hpactor::ActorAddress& addr) const noexcept {
        std::size_t seed = std::hash<hpactor::CommunicationEndpoint>{}(addr.endpoint);
        hpactor::ActorAddress::hash_combine(
            seed, std::hash<hpactor::ActorType>{}(addr.type));
        hpactor::ActorAddress::hash_combine(
            seed, std::hash<hpactor::ActorId>{}(addr.id));
        hpactor::ActorAddress::hash_combine(
            seed, std::hash<uint64_t>{}(addr.incarnation));
        return seed;
    }
};
```

- [ ] **Step 3: Run tests and fix**

Run: `ninja -C build && ./build/tests/net/test_actor_address`
Expected: PASS

- [ ] **Step 4: Commit**

```bash
git add include/hpactor/ref/actor_address.hpp tests/net/test_actor_address.cpp
git commit -m "refactor(actor_address): replace NodeId with CommunicationEndpoint"
```

---

## Task 3: Update Frame serialization for binary endpoint encoding

**Files:**
- Modify: `include/hpactor/net/frame.hpp`
- Modify: `src/net/frame.cpp`

- [ ] **Step 1: Write test for frame serialization**

Create `tests/net/test_frame_endpoint.cpp`:
```cpp
#include <hpactor/net/frame.hpp>
#include <hpactor/types/types.hpp>
#include <cassert>
#include <vector>

int main() {
    using namespace hpactor;

    // Test IPv4 endpoint encoding
    Ipv4Endpoint ipv4{0x01010101, 5353};  // 1.1.1.1
    bytes data = Frame::encode_endpoint(ipv4);
    assert(data.size() == 7);  // 1 + 4 + 2
    assert(data[0] == 0x04);   // IPv4 protocol byte

    // Test IPv4 endpoint decoding
    auto decoded = Frame::decode_endpoint(data);
    assert(std::holds_alternative<Ipv4Endpoint>(decoded));
    auto& decoded_ipv4 = std::get<Ipv4Endpoint>(decoded);
    assert(decoded_ipv4.addr == ipv4.addr);
    assert(decoded_ipv4.port() == 5353);

    // Test IPv6 endpoint encoding
    std::array<uint8_t, 16> loopback{};
    loopback[15] = 1;
    Ipv6Endpoint ipv6{loopback, 8080};
    bytes data6 = Frame::encode_endpoint(ipv6);
    assert(data6.size() == 19);  // 1 + 16 + 2
    assert(data6[0] == 0x06);     // IPv6 protocol byte

    // Test IPv6 endpoint decoding
    auto decoded6 = Frame::decode_endpoint(data6);
    assert(std::holds_alternative<Ipv6Endpoint>(decoded6));
    auto& decoded6_ipv6 = std::get<Ipv6Endpoint>(decoded6);
    assert(decoded6_ipv6.port() == 8080);

    // Test round-trip
    bytes encoded = Frame::encode_endpoint(CommunicationEndpoint{ipv4});
    auto round_trip = Frame::decode_endpoint(encoded);
    assert(round_trip == CommunicationEndpoint{ipv4});

    return 0;
}
```

Run: `ninja -C build && ./build/tests/net/test_frame_endpoint`
Expected: Compiles, may fail on missing Frame methods

- [ ] **Step 2: Update Frame with endpoint encoding**

Add to `include/hpactor/net/frame.hpp`:
```cpp
// Endpoint serialization (network byte order)
// Wire format: [protocol:1][addr:n][port:2]
// protocol: 0x04 = IPv4, 0x06 = IPv6
static bytes encode_endpoint(const CommunicationEndpoint& ep);
static CommunicationEndpoint decode_endpoint(bytes_view data);
```

Add to `src/net/frame.cpp`:
```cpp
#include <hpactor/types/types.hpp>
#include <cstring>

bytes Frame::encode_endpoint(const CommunicationEndpoint& ep) {
    bytes result;
    if (auto* ipv4 = std::get_if<Ipv4Endpoint>(&ep)) {
        result.push_back(0x04);  // IPv4 protocol byte
        result.resize(7);
        std::memcpy(&result[1], &ipv4->addr, 4);
        std::memcpy(&result[5], &ipv4->port, 2);
        return result;
    }
    if (auto* ipv6 = std::get_if<Ipv6Endpoint>(&ep)) {
        result.push_back(0x06);  // IPv6 protocol byte
        result.resize(19);
        std::memcpy(&result[1], ipv6->addr.data(), 16);
        std::memcpy(&result[17], &ipv6->port, 2);
        return result;
    }
    return result;
}

CommunicationEndpoint Frame::decode_endpoint(bytes_view data) {
    if (data.size() < 1) return Ipv4Endpoint{};

    uint8_t proto = data[0];
    if (proto == 0x04) {
        // IPv4: 1 + 4 + 2 = 7 bytes
        if (data.size() < 7) return Ipv4Endpoint{};
        uint32_t addr;
        uint16_t port;
        std::memcpy(&addr, &data[1], 4);
        std::memcpy(&port, &data[5], 2);
        return Ipv4Endpoint{addr, port};
    }
    if (proto == 0x06) {
        // IPv6: 1 + 16 + 2 = 19 bytes
        if (data.size() < 19) return Ipv6Endpoint{};
        std::array<uint8_t, 16> addr;
        uint16_t port;
        std::memcpy(addr.data(), &data[1], 16);
        std::memcpy(&port, &data[17], 2);
        return Ipv6Endpoint{addr, port};
    }
    return Ipv4Endpoint{};
}
```

- [ ] **Step 3: Run tests and fix**

Run: `ninja -C build && ./build/tests/net/test_frame_endpoint`
Expected: PASS

- [ ] **Step 4: Commit**

```bash
git add include/hpactor/net/frame.hpp src/net/frame.cpp tests/net/test_frame_endpoint.cpp
git commit -m "refactor(frame): add binary endpoint serialization"
```

---

## Task 4: Update TcpTransport to use CommunicationEndpoint

**Files:**
- Modify: `include/hpactor/net/transport.hpp`
- Modify: `include/hpactor/net/tcp_transport.hpp`
- Modify: `src/net/tcp_transport.cpp`

- [ ] **Step 1: Update transport.hpp interface**

Replace NodeId with CommunicationEndpoint in Transport base class. Change all method signatures that used `NodeId` to use `CommunicationEndpoint`.

- [ ] **Step 2: Update tcp_transport.hpp**

Replace all NodeId references with CommunicationEndpoint. Constructor takes `CommunicationEndpoint local_endpoint`.

- [ ] **Step 3: Update tcp_transport.cpp**

Change implementation to use endpoint directly for connection. Connect uses endpoint.to_sockaddr() for socket address.

- [ ] **Step 4: Run tests**

Run: `ninja -C build`
Expected: Compiles successfully

- [ ] **Step 5: Commit**

```bash
git add include/hpactor/net/transport.hpp include/hpactor/net/tcp_transport.hpp src/net/tcp_transport.cpp
git commit -m "refactor(tcp_transport): use CommunicationEndpoint instead of NodeId"
```

---

## Task 5: Update ConnectionPool to use CommunicationEndpoint

**Files:**
- Modify: `include/hpactor/net/connection_pool.hpp`
- Modify: `src/net/connection_pool.cpp`

- [ ] **Step 1: Update connection_pool.hpp**

Replace NodeId with CommunicationEndpoint.

- [ ] **Step 2: Update connection_pool.cpp**

Use `to_sockaddr()` for connect() call.

- [ ] **Step 3: Run tests and commit**

Run: `ninja -C build && ctest`
Expected: All tests pass

```bash
git add include/hpactor/net/connection_pool.hpp src/net/connection_pool.cpp
git commit -m "refactor(connection_pool): use CommunicationEndpoint for connections"
```

---

## Task 6: Update Registrar components

**Files:**
- Modify: `include/hpactor/net/registrar.hpp`
- Modify: `src/net/registrar.cpp`
- Modify: `src/net/registrar_server.cpp`
- Modify: `src/net/registrar_client.cpp`

- [ ] **Step 1: Update NodeRegistry in registrar.hpp**

Replace NodeId with CommunicationEndpoint in endpoint map.

- [ ] **Step 2: Update registrar.cpp, registrar_server.cpp, registrar_client.cpp**

Change all NodeId references to CommunicationEndpoint. Use endpoint serialization via Frame::encode_endpoint/decode_endpoint.

- [ ] **Step 3: Run tests and commit**

Run: `ninja -C build && ctest`
Expected: All tests pass

```bash
git add include/hpactor/net/registrar.hpp src/net/registrar.cpp src/net/registrar_server.cpp src/net/registrar_client.cpp
git commit -m "refactor(registrar): use CommunicationEndpoint for node endpoints"
```

---

## Task 7: Update Acceptor and remaining files

**Files:**
- Modify: `src/net/acceptor.cpp`
- Modify: `src/actor/actor_system.cpp`

- [ ] **Step 1: Update acceptor.cpp**

Use endpoint construction instead of string-based node_id. Build Ipv4Endpoint from parsed IP and port.

- [ ] **Step 2: Update actor_system.cpp**

Update NodeId usage throughout.

- [ ] **Step 3: Run full test suite**

Run: `ninja -C build && ctest --output-on-failure`
Expected: All tests pass

- [ ] **Step 4: Commit**

```bash
git add src/net/acceptor.cpp src/actor/actor_system.cpp
git commit -m "refactor(acceptor): use CommunicationEndpoint for local endpoint"
```

---

## Execution Options

**Plan complete and saved to `docs/superpowers/plans/2026-04-23-communication-endpoint-impl.md`. Two execution options:**

**1. Subagent-Driven (recommended)** - Dispatch fresh subagent per task, review between tasks, fast iteration

**2. Inline Execution** - Execute tasks in this session using executing-plans, batch execution with checkpoints

**Which approach?**