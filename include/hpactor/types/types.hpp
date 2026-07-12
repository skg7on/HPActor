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
#include <string_view>
#include <variant>
#include <vector>

#include <hpactor/adt/id.hpp>
#include <hpactor/adt/stream_buffer.hpp>
#include <hpactor/adt/tags.hpp>
#include <hpactor/msg/failure_reason.hpp>
#include <hpactor/msg/message_id.hpp>
#include <hpactor/msg/type_tag.hpp>
#include <vector>

namespace hpactor {

// constexpr network-to-host byte-order conversion (ntohs is not constexpr)
inline constexpr uint16_t net_to_host_u16(uint16_t net_val) noexcept {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return __builtin_bswap16(net_val);
#else
    return net_val;
#endif
}

// -----------------------------------------------------------------------------
// ActorId - unique identifier for an actor instance
// -----------------------------------------------------------------------------
using ActorId = Id<ActorTag>;

// -----------------------------------------------------------------------------
// Protocol - network protocol family
// -----------------------------------------------------------------------------
enum class Protocol { IPv4, IPv6 };

// -----------------------------------------------------------------------------
// DispatchPolicy — how an actor is dispatched to a scheduler worker
// -----------------------------------------------------------------------------
enum class DispatchPolicy : uint8_t {
    Cooperative = 0,
    DedicatedThread,
    DedicatedPool,
};

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
        return net_to_host_u16(port_nw);
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
    // addr stores the address in big-endian uint32_t format where
    // 0x7F000001 always means 127.0.0.1.  The MSB is the first octet.
    return (addr >> 24) == 0x7F;
}

[[nodiscard]] constexpr bool Ipv4Endpoint::is_private_network() const noexcept {
    auto b1 = static_cast<uint8_t>((addr >> 24) & 0xFF);
    auto b2 = static_cast<uint8_t>((addr >> 16) & 0xFF);
    return b1 == 10 ||
           (b1 == 172 && (b2 & 0xF0) == 16) ||
           (b1 == 192 && b2 == 168);
}

[[nodiscard]] constexpr bool Ipv4Endpoint::is_unspecified() const noexcept {
    return addr == 0;
}

inline void Ipv4Endpoint::to_sockaddr(sockaddr_in* out) const noexcept {
    out->sin_family = AF_INET;
    out->sin_port = port_nw;
    // addr is stored in internal big-endian format (0x7F000001 always
    // means 127.0.0.1).  Convert to platform network byte order for
    // the socket API.
    out->sin_addr.s_addr = addr;
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    out->sin_addr.s_addr = __builtin_bswap32(out->sin_addr.s_addr);
#endif
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
        return net_to_host_u16(port_nw);
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
inline void to_sockaddr(const EndPoint& ep, sockaddr* out, socklen_t* len) {
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

/// \brief Hash an EndPoint without allocating.
///
/// Computes a deterministic hash directly from the raw address and port
/// fields, avoiding the heap allocation incurred by to_string().
[[nodiscard]] inline std::size_t hash(const EndPoint& ep) noexcept {
    return std::visit(
        [](const auto& e) -> std::size_t {
            using T = std::decay_t<decltype(e)>;
            if constexpr (std::is_same_v<T, Ipv4Endpoint>) {
                std::size_t h = std::hash<uint32_t>{}(e.addr);
                h ^= std::hash<uint16_t>{}(e.port_nw) + 0x9e3779b9 + (h << 6) +
                     (h >> 2);
                return h;
            } else {
                // Ipv6Endpoint: hash the 16-byte address array and port.
                std::size_t h = 0;
                for (uint8_t b : e.addr) {
                    h ^= std::hash<uint8_t>{}(b) + 0x9e3779b9 + (h << 6) + (h >> 2);
                }
                h ^= std::hash<uint16_t>{}(e.port_nw) + 0x9e3779b9 + (h << 6) +
                     (h >> 2);
                return h;
            }
        },
        ep);
}
} // namespace endpoint_ops

// -----------------------------------------------------------------------------
// ActorType - type identifier for an actor
// -----------------------------------------------------------------------------
using ActorType = uint32_t;
constexpr ActorType InvalidActorType = 0;

// -----------------------------------------------------------------------------
// Well-known system actor identifiers
// -----------------------------------------------------------------------------

/// Actor ID of the SpawnReceiver system actor.
/// Handles remote spawn requests and routes SpawnRequest / SpawnResponse
/// messages.
constexpr ActorId SpawnReceiverId = ActorId(0xFFFF0001);

/// Type tag used in ActorAddress for system actors.
constexpr ActorType SystemActorType = 0xFFFF0000;

// -----------------------------------------------------------------------------
// incarnation_type - version counter for actor lifecycle
// -----------------------------------------------------------------------------
using incarnation_type = uint64_t;

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

    /// \brief Map this error's code to the canonical FailureReason.
    ///
    /// Maps the internal \c code_ to the corresponding FailureReason
    /// using the \c errors:: namespace constants.
    ///
    /// \return The FailureReason for the stored error code. Returns
    ///         \c FailureReason::Unknown when no mapping exists (e.g.
    ///         unmapped HTTP protocol codes, user-defined codes).
    [[nodiscard]] FailureReason failure_reason() const noexcept;

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
constexpr uint32_t invalid_argument = 6;
constexpr uint32_t cancelled = 7;

// HTTP protocol errors
constexpr uint32_t http_parse_error = 2001;
constexpr uint32_t http_connect_failed = 2002;
constexpr uint32_t http_timeout = 2003;

constexpr uint32_t user = 1000;
} // namespace errors

inline FailureReason error::failure_reason() const noexcept {
    switch (code_) {
        case errors::unknown:
            return FailureReason::Unknown;
        case errors::actor_down:
            return FailureReason::ActorDead;
        case errors::actor_not_found:
            return FailureReason::NoRoute;
        case errors::mailbox_full:
            return FailureReason::MailboxFull;
        case errors::timeout:
            return FailureReason::Timeout;
        case errors::invalid_argument:
            return FailureReason::RejectedByPolicy;
        case errors::cancelled:
            return FailureReason::Dropped; // Cancelled by caller — treated as
                                           // dropped
        default:
            return FailureReason::Unknown;
    }
}

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
    bool ok() const {
        return has_value_;
    }
    bool is_error() const {
        return !has_value_;
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
    bool ok() const {
        return has_value_;
    }
    bool is_error() const {
        return !has_value_;
    }
    void value() const {} // No-op for void
    const class error& error() const {
        return error_;
    }

  private:
    result() : has_value_(true) {}
    result(class error err) : has_value_(false), error_(std::move(err)) {}

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
using AlarmHandle = Id<AlarmTag>;

// -----------------------------------------------------------------------------
// Trace identifiers - W3C/OpenTelemetry-compatible distributed tracing IDs
// -----------------------------------------------------------------------------
struct TraceId {
    std::array<uint8_t, 16> bytes{};

    bool valid() const noexcept {
        for (uint8_t b : bytes) {
            if (b != 0) {
                return true;
            }
        }
        return false;
    }

    bool operator==(const TraceId& other) const noexcept {
        return bytes == other.bytes;
    }
};

struct SpanId {
    std::array<uint8_t, 8> bytes{};

    bool valid() const noexcept {
        for (uint8_t b : bytes) {
            if (b != 0) {
                return true;
            }
        }
        return false;
    }

    bool operator==(const SpanId& other) const noexcept {
        return bytes == other.bytes;
    }
};

struct TraceFlags {
    static constexpr uint8_t kSampled = 0x01;

    uint8_t value = 0;

    bool sampled() const noexcept {
        return (value & kSampled) != 0;
    }

    void set_sampled(bool enabled) noexcept {
        if (enabled) {
            value = static_cast<uint8_t>(value | kSampled);
        } else {
            value = static_cast<uint8_t>(value & ~kSampled);
        }
    }
};

struct TraceContext {
    TraceId trace_id;
    SpanId span_id;
    TraceFlags flags;
    uint8_t version = 0;
    uint16_t tracestate_len = 0;
    std::array<char, 256> tracestate{};

    bool valid() const noexcept {
        return trace_id.valid() && span_id.valid();
    }

    bool sampled() const noexcept {
        return flags.sampled();
    }

    std::string_view tracestate_view() const noexcept {
        return {tracestate.data(), tracestate_len};
    }

    void clear() noexcept {
        trace_id = TraceId{};
        span_id = SpanId{};
        flags = TraceFlags{};
        version = 0;
        tracestate_len = 0;
        tracestate.fill('\0');
    }
};

// -----------------------------------------------------------------------------
// StreamBuffer - byte buffer type
// -----------------------------------------------------------------------------
using adt::StreamBuffer;

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
