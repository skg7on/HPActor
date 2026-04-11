#pragma once

#include <functional>
#include <hpactor/types.hpp>

namespace hpactor {

// -----------------------------------------------------------------------------
// ActorAddress - unique identifier for an actor across the distributed system
// -----------------------------------------------------------------------------
struct ActorAddress {
    NodeId node_id = 0;       // Network location (0 for local)
    ActorType type = 0;      // Actor type identifier
    ActorId id;               // Unique instance ID
    uint64_t incarnation = 0; // Increments on restart

    ActorAddress() = default;
    ActorAddress(NodeId node, ActorType t, ActorId i, uint64_t inc)
        : node_id(node), type(t), id(i), incarnation(inc) {}

    bool operator==(const ActorAddress& other) const {
        return node_id == other.node_id && type == other.type && id == other.id
               && incarnation == other.incarnation;
    }
    bool operator!=(const ActorAddress& other) const { return !(*this == other); }
    bool is_local() const { return node_id == InvalidNodeId; }
    explicit operator bool() const { return id.value() != 0; }
};

using ActorAddr = ActorAddress;
constexpr ActorAddr invalid_actor_addr{};

} // namespace hpactor

// -----------------------------------------------------------------------------
// std::hash specialization for ActorId
// -----------------------------------------------------------------------------
template<>
struct std::hash<hpactor::ActorId> {
    std::size_t operator()(const hpactor::ActorId& aid) const noexcept {
        return std::hash<hpactor::ActorId::counter_type>{}(aid.value());
    }
};

// -----------------------------------------------------------------------------
// std::hash specialization for ActorAddress
// -----------------------------------------------------------------------------
template<>
struct std::hash<hpactor::ActorAddress> {
    std::size_t operator()(const hpactor::ActorAddress& addr) const noexcept {
        std::size_t h1 = std::hash<hpactor::NodeId>{}(addr.node_id);
        std::size_t h2 = std::hash<hpactor::ActorType>{}(addr.type);
        std::size_t h3 = std::hash<hpactor::ActorId>{}(addr.id);
        std::size_t h4 = std::hash<uint64_t>{}(addr.incarnation);
        // Combine hashes
        return h1 ^ (h2 << 1) ^ (h3 << 2) ^ (h4 << 3);
    }
};