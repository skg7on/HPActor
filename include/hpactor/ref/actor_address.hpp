#pragma once

#include <functional>
#include <hpactor/types.hpp>

namespace hpactor {

// -----------------------------------------------------------------------------
// ActorAddress - unique identifier for an actor across the distributed system
// -----------------------------------------------------------------------------
struct ActorAddress {
    NodeId node_id = 0;       // Network location (0 for local)
    ActorType type = 0;       // Actor type identifier
    ActorId id;               // Unique instance ID
    uint64_t incarnation = 0; // Increments on restart

    ActorAddress() = default;
    ActorAddress(NodeId node, ActorType t, ActorId i, uint64_t inc)
        : node_id(node), type(t), id(i), incarnation(inc) {}

    bool operator==(const ActorAddress& other) const noexcept {
        return node_id == other.node_id && type == other.type &&
               id == other.id && incarnation == other.incarnation;
    }
    bool operator!=(const ActorAddress& other) const noexcept {
        return !(*this == other);
    }
    bool is_local() const noexcept {
        return node_id == InvalidNodeId;
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
constexpr ActorAddr invalid_actor_addr{};

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