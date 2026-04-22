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

#include <hpactor/core/actor_system_ids.hpp>
#include <hpactor/ref/actor_address.hpp>
#include <hpactor/ref/actor_ref.hpp>
#include <hpactor/types/types.hpp>

#include <condition_variable>
#include <memory>
#include <mutex>
#include <variant>

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
    ActorAddress supervisor_addr;  // supervisor's address for link establishment
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
// SpawnMessageVariant - variant for spawn protocol messages only
// This is separate from the main MessageVariant to avoid circular includes
// -----------------------------------------------------------------------------
using SpawnMessageVariant = std::variant<SpawnRequest, SpawnResponse>;

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

    // Message ID for correlation with response
    void set_message_id(uint64_t id) { message_id_ = id; }
    uint64_t message_id() const { return message_id_; }

private:
    NodeId node_id_ = "";
    std::chrono::milliseconds timeout_{5000};
    mutable std::unique_ptr<std::mutex> mutex_;
    std::unique_ptr<std::condition_variable> cv_;
    bool ready_ = false;
    bool cancelled_ = false;
    SpawnResponse response_{};
    uint64_t message_id_ = 0;  // For correlation with response
};

} // namespace hpactor