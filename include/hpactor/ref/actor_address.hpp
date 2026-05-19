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

// -----------------------------------------------------------------------------
// ActorAddress - unique identifier for an actor across the distributed system
// -----------------------------------------------------------------------------
struct ActorAddress {
    EndPoint endpoint;        // Network location
    ActorType type = 0;       // Actor type identifier
    ActorId id;               // Unique instance ID
    uint64_t incarnation = 0; // Increments on restart

    ActorAddress() : endpoint(Ipv4Endpoint{0x7F000001, 0}) {}
    ActorAddress(EndPoint ep, ActorType t, ActorId i, uint64_t inc)
        : endpoint(std::move(ep)), type(t), id(i), incarnation(inc) {}

    bool operator==(const ActorAddress& other) const noexcept {
        return endpoint == other.endpoint && type == other.type &&
               id == other.id && incarnation == other.incarnation;
    }
    bool operator!=(const ActorAddress& other) const noexcept {
        return !(*this == other);
    }
    bool is_local() const noexcept;
    [[nodiscard]] std::string to_string() const;
    explicit operator bool() const {
        return id.value() != 0;
    }

  public:
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

using ActorAddr = ActorAddress;
inline const ActorAddr invalid_actor_addr{};

} // namespace hpactor

// -----------------------------------------------------------------------------
// std::hash specialization for ActorAddress
// -----------------------------------------------------------------------------
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