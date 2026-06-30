// Copyright 2026 HPActor Contributors
// (Apache 2.0 license header)
#pragma once

#include <cstdint>

#include <hpactor/core/actor_id.hpp>
#include <hpactor/ref/actor_address.hpp>
#include <hpactor/types/types.hpp>

namespace hpactor {

/// \brief Peer-qualified stream identity.
struct StreamKey {
    EndPoint peer;
    uint64_t stream_id{0};

    friend bool operator==(const StreamKey& a, const StreamKey& b) = default;
};

} // namespace hpactor

/// \brief std::hash specialization for \c StreamKey.
template <> struct std::hash<hpactor::StreamKey> {
    std::size_t operator()(const hpactor::StreamKey& k) const noexcept {
        std::size_t h1 = std::hash<uint64_t>{}(k.stream_id);
        // Hash EndPoint via its string representation
        std::size_t h2 =
            std::hash<std::string>{}(hpactor::endpoint_ops::to_string(k.peer));
        return h1 ^ (h2 + 0x9e3779b9 + (h1 << 6) + (h1 >> 2));
    }
};

namespace hpactor {

/// \brief Stream registry session state.
enum class StreamSessionState : uint8_t {
    Opening, ///< Reserve; spawn in progress.
    Active,  ///< Fully operational.
    Closing, ///< Terminal message dispatched, removal pending.
};

/// \brief Per-stream session record.
struct StreamSession {
    StreamSessionState state{StreamSessionState::Opening};
    ActorId sender_actor{};
    ActorId receiver_actor{};
    ActorId target_actor{};
    uint64_t generation{0};
};

/// \brief Bounded read-only snapshot of stream registry state.
///
/// Contains registry-owned facts only — peer, stream id, state,
/// and actor ids.  Does not read sender/receiver mutable window
/// or in-flight fields.
struct StreamRuntimeSnapshot {
    /// Per-session record in the snapshot.
    struct Record {
        StreamKey key;
        StreamSessionState state;
        ActorId sender_actor;
        ActorId receiver_actor;
        ActorId target_actor;
    };

    std::vector<Record> sessions;
    uint32_t active_count{0};
    uint32_t opening_count{0};
    uint32_t max_active{0};
};

} // namespace hpactor
