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
    NodeId node_id;           // Network location ("host:port" or "" for local)
    ActorType type = 0;       // Actor type identifier
    ActorId id;               // Unique instance ID
    uint64_t incarnation = 0;  // Increments on restart

    ActorAddress() = default;
    ActorAddress(NodeId node, ActorType t, ActorId i, uint64_t inc)
        : node_id(std::move(node)), type(t), id(i), incarnation(inc) {}

    bool operator==(const ActorAddress& other) const noexcept {
        return node_id == other.node_id && type == other.type &&
               id == other.id && incarnation == other.incarnation;
    }
    bool operator!=(const ActorAddress& other) const noexcept {
        return !(*this == other);
    }
    bool is_local() const noexcept {
        return is_local_node_id(node_id);
    }
    explicit operator bool() const {
        return id.value() != 0;
    }

  public:
    static void hash_combine(size_t& seed, size_t value) noexcept {
        seed ^= value + 0x9e3779b9 + (seed << 6) + (seed >> 2);
    }
};

using ActorAddr = ActorAddress;
inline const ActorAddr invalid_actor_addr{};

} // namespace hpactor

// -----------------------------------------------------------------------------
// std::hash specialization for ActorId
// -----------------------------------------------------------------------------
template <> struct std::hash<hpactor::ActorId> {
    std::size_t operator()(const hpactor::ActorId& aid) const noexcept {
        size_t seed = std::hash<hpactor::ActorId::counter_type>{}(aid.value());
        hpactor::ActorAddress::hash_combine(seed, aid.value());
        return seed;
    }
};

// -----------------------------------------------------------------------------
// std::hash specialization for ActorAddress
// -----------------------------------------------------------------------------
template <> struct std::hash<hpactor::ActorAddress> {
    std::size_t operator()(const hpactor::ActorAddress& addr) const noexcept {
        std::size_t seed = std::hash<hpactor::NodeId>{}(addr.node_id);
        hpactor::ActorAddress::hash_combine(
            seed, std::hash<hpactor::ActorType>{}(addr.type));
        hpactor::ActorAddress::hash_combine(
            seed, std::hash<hpactor::ActorId>{}(addr.id));
        hpactor::ActorAddress::hash_combine(
            seed, std::hash<uint64_t>{}(addr.incarnation));
        return seed;
    }
};