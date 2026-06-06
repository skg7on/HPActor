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
#include <hpactor/types/failure_reason.hpp>
#include <hpactor/types/request_handle.hpp>
#include <hpactor/types/types.hpp>

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

/// \brief Map a spawn_errors code to the canonical FailureReason.
///
/// Every spawn error code maps to a corresponding FailureReason in the
/// spawn range (90-99), a route/timeout/lifecycle code, or Unknown for
/// a success code.
///
/// \param[in] spawn_error_code A spawn_errors constant.
/// \return The canonical FailureReason. \c spawn_errors::success maps to
///         \c FailureReason::Unknown (not a failure).
[[nodiscard]] constexpr FailureReason
failure_reason(uint32_t spawn_error_code) noexcept {
    switch (spawn_error_code) {
        case spawn_errors::success:
            return FailureReason::Unknown;
        case spawn_errors::unknown_type:
            return FailureReason::NoRoute;
        case spawn_errors::deserialization_failed:
            return FailureReason::SerializationError;
        case spawn_errors::node_unreachable:
            return FailureReason::NodeUnavailable;
        case spawn_errors::timeout:
            return FailureReason::Timeout;
        case spawn_errors::spawn_receiver_not_running:
            return FailureReason::ActorNotReady;
        default:
            return FailureReason::SpawnFailed;
    }
}

// -----------------------------------------------------------------------------
// SpawnRequest - sent from caller to spawn receiver on remote node
// Serialized via protobuf messages in messages.proto
// -----------------------------------------------------------------------------
struct SpawnRequest {
    std::string actor_type_name;  // e.g., "calculator"
    TypeTag args_type;            // type tag for deserializing args
    StreamBuffer serialized_args; // type-erased constructor arguments
    ActorAddress supervisor_addr; // supervisor's address for link establishment
};

// -----------------------------------------------------------------------------
// SpawnResponse - sent back from spawn receiver to caller
// Serialized via protobuf messages in messages.proto
// -----------------------------------------------------------------------------
struct SpawnResponse {
    ActorAddress actor_addr; // new actor's address (node_id, type, id,
                             // incarnation)
    uint32_t error_code;     // spawn_errors::code
};

// -----------------------------------------------------------------------------
// AsyncActor — backward-compatible alias for asynchronous remote spawn result
// -----------------------------------------------------------------------------
// \deprecated Use \c RequestHandle<ActorRef> directly. This alias is kept for
//             source compatibility with callers that stored the return value of
//             \c ActorSystem::spawn_remote_async() as \c AsyncActor.
using AsyncActor = RequestHandle<ActorRef>;

} // namespace hpactor