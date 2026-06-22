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

#include <functional>
#include <hpactor/types/types.hpp>

namespace hpactor {

/// \brief Globally-unique, network-addressable actor identity.
///
/// Composed of a network endpoint, type tag, instance ID, and incarnation
/// counter. The incarnation increments on restart, allowing stale references
/// to be detected.
///
/// \note \c ActorAddress{ } defaults to loopback (127.0.0.1:0) to match
///       \c parse_endpoint("").
struct ActorAddress {
    EndPoint endpoint;        ///< Network location (IPv4 or IPv6 endpoint).
    ActorType type = 0;       ///< Actor type identifier.
    ActorId id;               ///< Unique instance ID.
    uint64_t incarnation = 0; ///< Monotonic counter incremented on restart.

    /// \brief Default-construct with loopback endpoint.
    ActorAddress() : endpoint(Ipv4Endpoint{0x7F000001, 0}) {}

    /// \brief Construct with explicit fields.
    /// \param[in] ep Network endpoint.
    /// \param[in] t Actor type tag.
    /// \param[in] i Instance ID.
    /// \param[in] inc Incarnation counter.
    ActorAddress(EndPoint ep, ActorType t, ActorId i, uint64_t inc)
        : endpoint(std::move(ep)), type(t), id(i), incarnation(inc) {}

    bool operator==(const ActorAddress& other) const noexcept {
        return endpoint == other.endpoint && type == other.type &&
               id == other.id && incarnation == other.incarnation;
    }
    bool operator!=(const ActorAddress& other) const noexcept {
        return !(*this == other);
    }

    /// \brief Returns \c true if the actor resides on the local node.
    ///
    /// Checks for loopback addresses (127.0.0.1 or ::1).
    bool is_local() const noexcept;

    /// \brief Human-readable string representation of the endpoint.
    [[nodiscard]] std::string to_string() const;

    /// \brief Returns a string identifier for the node hosting this actor.
    [[nodiscard]] std::string node_id() const;

    /// \brief Returns \c true if the actor ID is non-zero.
    explicit operator bool() const {
        return id.value() != 0;
    }

    /// \brief Hash combine utility used by \c std::hash<ActorAddress>.
    static void hash_combine(size_t& seed, size_t value) noexcept {
        seed ^= value + 0x9e3779b9 + (seed << 6) + (seed >> 2);
    }
};

inline bool ActorAddress::is_local() const noexcept {
    if (auto* ipv4 = std::get_if<Ipv4Endpoint>(&endpoint)) {
        return ipv4->is_loopback();
    }
    if (auto* ipv6 = std::get_if<Ipv6Endpoint>(&endpoint)) {
        return ipv6->is_loopback();
    }
    return true; // Empty variant is considered local
}

inline std::string ActorAddress::to_string() const {
    return endpoint_ops::to_string(endpoint);
}

inline std::string ActorAddress::node_id() const {
    return endpoint_ops::to_string(endpoint);
}

/// \brief Shorthand alias for \c ActorAddress.
using ActorAddr = ActorAddress;
/// \brief Sentinel value representing an invalid/unset address.
inline const ActorAddr invalid_actor_addr{};

} // namespace hpactor

/// \brief \c std::hash specialization for \c hpactor::ActorAddress.
///
/// Hashes the endpoint, type, ID, and incarnation fields.
template <> struct std::hash<hpactor::ActorAddress> {
    std::size_t operator()(const hpactor::ActorAddress& addr) const noexcept {
        std::size_t seed = std::hash<hpactor::EndPoint>{}(addr.endpoint);
        hpactor::ActorAddress::hash_combine(
            seed, std::hash<hpactor::ActorType>{}(addr.type));
        hpactor::ActorAddress::hash_combine(
            seed, std::hash<hpactor::ActorId>{}(addr.id));
        hpactor::ActorAddress::hash_combine(
            seed, std::hash<uint64_t>{}(addr.incarnation));
        return seed;
    }
};
