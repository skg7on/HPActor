#pragma once

#include <hpactor/core/actor_system_ids.hpp>
#include <hpactor/ref/actor_address.hpp>
#include <hpactor/ref/actor_ref.hpp>
#include <hpactor/types/serialization.hpp>
#include <hpactor/types/types.hpp>

#include <condition_variable>
#include <memory>
#include <mutex>

namespace hpactor {

// -----------------------------------------------------------------------------
// Spawn-specific error codes (separate from general errors namespace)
// -----------------------------------------------------------------------------
namespace spawn_errors {
constexpr uint32_t success = 0;
constexpr uint32_t unknown_type = 1;
constexpr uint32_t deserialization_failed = 2;
constexpr uint32_t node_unreachable = 3;
constexpr uint32_t timeout = 4;
constexpr uint32_t spawn_receiver_not_running = 5;
} // namespace spawn_errors

// -----------------------------------------------------------------------------
// SpawnRequest - sent from caller to spawn receiver on remote node
// Note: This becomes part of MessageVariant via serialization.hpp
// -----------------------------------------------------------------------------
struct SpawnRequest {
    std::string actor_type_name;    // e.g., "calculator"
    TypeTag args_type;            // type tag for deserializing args
    bytes serialized_args;         // type-erased constructor arguments
};

// -----------------------------------------------------------------------------
// SpawnResponse - sent back from spawn receiver to caller
// Note: This becomes part of MessageVariant via serialization.hpp
// -----------------------------------------------------------------------------
struct SpawnResponse {
    ActorAddress actor_addr;    // new actor's address (node_id, type, id, incarnation)
    uint32_t error_code;       // spawn_errors::code
};

// -----------------------------------------------------------------------------
// AsyncActor - handle for asynchronous remote spawn
// -----------------------------------------------------------------------------
// Allows non-blocking spawn with result retrieval via get().
// WARNING: get() blocks the calling thread.
class AsyncActor {
public:
    AsyncActor();
    AsyncActor(AsyncActor&& other) noexcept;
    AsyncActor& operator=(AsyncActor&& other) noexcept;

    // Construct with node_id and timeout
    AsyncActor(NodeId node_id, std::chrono::milliseconds timeout);

    // Wait for response and return result (blocks until response or timeout)
    result<ActorRef> get();

    // Check if response received (non-blocking)
    bool ready() const;

    // Cancel pending spawn
    void cancel();

    // Get associated node ID
    NodeId node_id() const { return node_id_; }

    // Set response (called by transport layer when response received)
    void set_response(SpawnResponse response);

private:
    NodeId node_id_ = 0;
    std::chrono::milliseconds timeout_{5000};
    mutable std::unique_ptr<std::mutex> mutex_;
    std::unique_ptr<std::condition_variable> cv_;
    bool ready_ = false;
    bool cancelled_ = false;
    SpawnResponse response_{};
};

} // namespace hpactor