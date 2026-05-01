// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include <arpa/inet.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <netinet/in.h>
#include <string>
#include <variant>
#include <vector>

#include <hpactor/adt/stream_buffer.hpp>
#include <vector>

namespace hpactor {

// -----------------------------------------------------------------------------
// ActorId - unique identifier for an actor instance
// -----------------------------------------------------------------------------
struct ActorId {
    using counter_type = uint64_t;

    ActorId() = default;

    explicit constexpr ActorId(counter_type value) : value_(value) {}

    counter_type value() const noexcept {
        return value_;
    }

    bool operator==(const ActorId& other) const noexcept {
        return value_ == other.value_;
    }
    bool operator!=(const ActorId& other) const noexcept {
        return !(*this == other);
    }

  private:
    counter_type value_ = 0;
};

// -----------------------------------------------------------------------------
// Protocol - network protocol family
// -----------------------------------------------------------------------------
enum class Protocol { IPv4, IPv6 };

// -----------------------------------------------------------------------------
// Ipv4Endpoint - IPv4 address and port (network byte order)
// -----------------------------------------------------------------------------
struct Ipv4Endpoint {
    uint32_t addr;    // Network byte order (big-endian)
    uint16_t port_nw; // Network byte order

    constexpr Ipv4Endpoint() noexcept : addr(0), port_nw(0) {}
    constexpr Ipv4Endpoint(uint32_t a, uint16_t p) noexcept
        : addr(a), port_nw(p) {}

    [[nodiscard]] constexpr uint16_t port() const noexcept {
        return ntohs(port_nw);
    }
    [[nodiscard]] constexpr bool is_ipv4() const noexcept {
        return true;
    }
    [[nodiscard]] constexpr bool is_ipv6() const noexcept {
        return false;
    }
    [[nodiscard]] constexpr bool is_loopback() const noexcept;
    [[nodiscard]] constexpr bool is_private_network() const noexcept;
    [[nodiscard]] constexpr bool is_unspecified() const noexcept;

    // For socket operations
    constexpr socklen_t sockaddr_length() const noexcept {
        return sizeof(sockaddr_in);
    }
    void to_sockaddr(sockaddr_in* out) const noexcept;

    constexpr bool operator==(const Ipv4Endpoint& other) const noexcept {
        return addr == other.addr && port_nw == other.port_nw;
    }
    constexpr bool operator!=(const Ipv4Endpoint& other) const noexcept {
        return !(*this == other);
    }
};

[[nodiscard]] constexpr bool Ipv4Endpoint::is_loopback() const noexcept {
    // Network byte order: 127.0.0.1 = 0x7F000001
    // MSB (byte 0 in network order) = 0x7F
    // On little-endian, addr value is still 0x7F000001, so MSB = (addr >> 24)
    // On big-endian, MSB = (addr >> 24) = 0x7F
    // Both give same result
    return (addr >> 24) == 0x7F;
}

[[nodiscard]] constexpr bool Ipv4Endpoint::is_private_network() const noexcept {
    uint8_t b1 = (addr >> 24) & 0xFF;
    uint32_t rest = addr & 0xFFFF0000;
    return b1 == 10 || (b1 == 172 && ((addr >> 16) & 0xFF & 0xF0) == 0x10) ||
           (b1 == 192 && rest == 0xC0A80000);
}

[[nodiscard]] constexpr bool Ipv4Endpoint::is_unspecified() const noexcept {
    return addr == 0;
}

inline void Ipv4Endpoint::to_sockaddr(sockaddr_in* out) const noexcept {
    out->sin_family = AF_INET;
    out->sin_port = port_nw;
    out->sin_addr.s_addr = addr;
    std::memset(out->sin_zero, 0, sizeof(out->sin_zero));
}

// -----------------------------------------------------------------------------
// Ipv6Endpoint - IPv6 address and port (network byte order)
// -----------------------------------------------------------------------------
struct Ipv6Endpoint {
    std::array<uint8_t, 16> addr; // Network byte order
    uint16_t port_nw;             // Network byte order

    constexpr Ipv6Endpoint() noexcept : addr{}, port_nw(0) {}
    constexpr Ipv6Endpoint(std::array<uint8_t, 16> a, uint16_t p) noexcept
        : addr(a), port_nw(p) {}

    [[nodiscard]] constexpr uint16_t port() const noexcept {
        return ntohs(port_nw);
    }
    [[nodiscard]] constexpr bool is_ipv4() const noexcept {
        return false;
    }
    [[nodiscard]] constexpr bool is_ipv6() const noexcept {
        return true;
    }
    [[nodiscard]] bool is_loopback() const noexcept;
    [[nodiscard]] bool is_private_network() const noexcept;
    [[nodiscard]] bool is_unspecified() const noexcept;

    // For socket operations
    constexpr socklen_t sockaddr_length() const noexcept {
        return sizeof(sockaddr_in6);
    }
    void to_sockaddr(sockaddr_in6* out) const noexcept;

    bool operator==(const Ipv6Endpoint& other) const noexcept {
        return addr == other.addr && port_nw == other.port_nw;
    }
    bool operator!=(const Ipv6Endpoint& other) const noexcept {
        return !(*this == other);
    }
};

inline bool Ipv6Endpoint::is_loopback() const noexcept {
    for (std::size_t i = 0; i < 15; ++i)
        if (addr[i] != 0)
            return false;
    return addr[15] == 1;
}

inline bool Ipv6Endpoint::is_private_network() const noexcept {
    return (addr[0] & 0xFE) == 0xFC;
}

inline bool Ipv6Endpoint::is_unspecified() const noexcept {
    for (std::size_t i = 0; i < 16; ++i)
        if (addr[i] != 0)
            return false;
    return true;
}

inline void Ipv6Endpoint::to_sockaddr(sockaddr_in6* out) const noexcept {
    out->sin6_family = AF_INET6;
    out->sin6_port = port_nw;
    std::memcpy(out->sin6_addr.s6_addr, addr.data(), 16);
    out->sin6_flowinfo = 0;
    out->sin6_scope_id = 0;
}

// -----------------------------------------------------------------------------
// EndPoint - variant over IPv4/IPv6
// -----------------------------------------------------------------------------
using EndPoint = std::variant<Ipv4Endpoint, Ipv6Endpoint>;

// -----------------------------------------------------------------------------
// LocalEndpoint - loopback endpoint for local actor communication
// -----------------------------------------------------------------------------
inline constexpr Ipv4Endpoint LocalEndpoint{0x7F000001, 0}; // 127.0.0.1:0 in
                                                            // network byte
                                                            // order

// to_sockaddr free function for variant
inline void
to_sockaddr(const EndPoint& ep, sockaddr* out, socklen_t* len) {
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
// EndPoint operations (implemented in cpp)
// -----------------------------------------------------------------------------
namespace endpoint_ops {
[[nodiscard]] Protocol protocol(const EndPoint& ep);
[[nodiscard]] int address_family(const EndPoint& ep);
[[nodiscard]] std::string to_string(const EndPoint& ep);
[[nodiscard]] EndPoint parse_endpoint(std::string_view node_id);
} // namespace endpoint_ops

// -----------------------------------------------------------------------------
// ActorType - type identifier for an actor
// -----------------------------------------------------------------------------
using ActorType = uint32_t;
constexpr ActorType InvalidActorType = 0;

// -----------------------------------------------------------------------------
// incarnation_type - version counter for actor lifecycle
// -----------------------------------------------------------------------------
using incarnation_type = uint64_t;

// -----------------------------------------------------------------------------
// MessageId - unique identifier for a message
// -----------------------------------------------------------------------------
struct MessageId {
    using counter_type = uint64_t;

    MessageId() = default;

    explicit MessageId(counter_type value) : value_(value) {}

    counter_type value() const {
        return value_;
    }

    bool operator==(const MessageId& other) const {
        return value_ == other.value_;
    }
    bool operator!=(const MessageId& other) const {
        return !(*this == other);
    }

    static MessageId generate() {
        return MessageId(next_id_.fetch_add(1));
    }

  private:
    counter_type value_ = 0;
    static std::atomic<uint64_t> next_id_;
};

// Definition of static member
inline std::atomic<uint64_t> MessageId::next_id_{1};

// -----------------------------------------------------------------------------
// error - error code wrapper (no exceptions in hot path)
// -----------------------------------------------------------------------------
class error {
  public:
    error() = default;

    explicit error(uint32_t code, std::string msg = {})
        : code_(code), message_(std::move(msg)) {}

    uint32_t code() const {
        return code_;
    }
    const std::string& message() const {
        return message_;
    }

    bool ok() const {
        return code_ == 0;
    }
    explicit operator bool() const {
        return !ok();
    }

  private:
    uint32_t code_ = 0;
    std::string message_;
};

// -----------------------------------------------------------------------------
// errors namespace - canonical error codes
// -----------------------------------------------------------------------------
namespace errors {
constexpr uint32_t unknown = 1;
constexpr uint32_t actor_down = 2;
constexpr uint32_t actor_not_found = 3;
constexpr uint32_t mailbox_full = 4;
constexpr uint32_t timeout = 5;

// HTTP protocol errors
constexpr uint32_t http_parse_error = 2001;
constexpr uint32_t http_connect_failed = 2002;
constexpr uint32_t http_timeout = 2003;

constexpr uint32_t user = 1000;
} // namespace errors

// -----------------------------------------------------------------------------
// result<T> - return type for message handlers
// -----------------------------------------------------------------------------
template <typename T> class result {
  public:
    static result<T> make(T&& value) {
        return result<T>(std::move(value));
    }
    static result<T> make(class error err) {
        return result<T>(std::move(err));
    }

    bool has_value() const {
        return has_value_;
    }
    T& value() {
        return std::get<0>(value_);
    }
    const class error& error() const {
        return std::get<1>(value_);
    }

  private:
    result(T&& val) : has_value_(true), value_(std::move(val)) {}
    result(class error err) : has_value_(false), value_(err) {}

    bool has_value_;
    std::variant<T, class error> value_;
};

template <> class result<void> {
  public:
    static result<void> make() {
        return result<void>();
    }
    static result<void> make(class error err) {
        return result<void>(std::move(err));
    }

    bool has_value() const {
        return has_value_;
    }
    void value() const {} // No-op for void
    const class error& error() const {
        return error_;
    }

  private:
    result<void>() : has_value_(true) {}
    result<void>(class error err) : has_value_(false), error_(std::move(err)) {}

    bool has_value_;
    class error error_;
};

// -----------------------------------------------------------------------------
// Clock - for time-based operations
// -----------------------------------------------------------------------------
class Clock {
  public:
    using time_point = std::chrono::steady_clock::time_point;
    using duration = std::chrono::milliseconds;
    time_point now() const {
        return std::chrono::steady_clock::now();
    }
};

// -----------------------------------------------------------------------------
// AlarmHandle - opaque handle for alarms
// -----------------------------------------------------------------------------
struct AlarmHandle {
    AlarmHandle() = default;

    explicit AlarmHandle(uint64_t id) : id_(id) {}

    uint64_t id() const {
        return id_;
    }

  private:
    uint64_t id_ = 0;
};

// -----------------------------------------------------------------------------
// TraceContext - for distributed tracing
// -----------------------------------------------------------------------------
struct TraceContext {
    TraceContext() = default;

    TraceContext(uint64_t trace_id, uint64_t span_id, uint32_t flags)
        : trace_id_(trace_id), span_id_(span_id), flags_(flags) {}

    uint64_t trace_id() const {
        return trace_id_;
    }
    uint64_t span_id() const {
        return span_id_;
    }
    uint32_t flags() const {
        return flags_;
    }

  private:
    uint64_t trace_id_ = 0;
    uint64_t span_id_ = 0;
    uint32_t flags_ = 0;
};

// -----------------------------------------------------------------------------
// bytes - byte buffer type
// -----------------------------------------------------------------------------
using bytes = adt::StreamBuffer;

// -----------------------------------------------------------------------------
// TypeTag - type identifier for serialization (replaces RTTI)
// Each serializable type gets a unique tag. System messages use tags 0-99,
// user messages use tags 100+.
// -----------------------------------------------------------------------------
enum class TypeTag : uint32_t {
    Invalid = 0,

    // System messages (always present)
    DownMsg = 1,
    ExitMsg = 2,
    LinkMsg = 3,
    UnlinkMsg = 4,
    MonitorMsg = 10,
    DemonitorMsg = 11,

    // Spawn protocol (Phase 8)
    SpawnRequestTag = 5,
    SpawnResponseTag = 6,
    ErrorMsg = 7,

    // HTTP protocol (Phase 11)
    HttpRequestTag = 8,
    HttpResponseTag = 9,

    // First available user tag
    User = 100,
};

} // namespace hpactor

// -----------------------------------------------------------------------------
// std::hash specializations (must be in namespace std)
// -----------------------------------------------------------------------------
template <> struct std::hash<hpactor::Ipv4Endpoint> {
    std::size_t operator()(const hpactor::Ipv4Endpoint& ep) const noexcept {
        std::size_t h = std::hash<uint32_t>{}(ep.addr);
        h ^= std::hash<uint16_t>{}(ep.port()) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};

template <> struct std::hash<hpactor::Ipv6Endpoint> {
    std::size_t operator()(const hpactor::Ipv6Endpoint& ep) const noexcept {
        std::size_t h = 0;
        for (uint8_t b : ep.addr) {
            h ^= std::hash<uint8_t>{}(b) + 0x9e3779b9 + (h << 6) + (h >> 2);
        }
        h ^= std::hash<uint16_t>{}(ep.port()) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};

template <> struct std::hash<hpactor::EndPoint> {
    std::size_t operator()(const hpactor::EndPoint& ep) const noexcept {
        if (auto* ipv4 = std::get_if<hpactor::Ipv4Endpoint>(&ep)) {
            return std::hash<hpactor::Ipv4Endpoint>{}(*ipv4);
        }
        return std::hash<hpactor::Ipv6Endpoint>{}(
            std::get<hpactor::Ipv6Endpoint>(ep));
    }
};
