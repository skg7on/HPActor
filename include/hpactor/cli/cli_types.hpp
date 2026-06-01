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

#include <cstdint>
#include <string>

namespace hpactor {
namespace cli {

/// \brief Lightweight actor metadata for CLI introspection.
///
/// Named distinctly from protobuf-generated classes in cli_messages.pb.h
/// to avoid ODR collisions when both headers are included in the same
/// translation unit.
struct ActorMeta {
    uint64_t actor_id = 0;           ///< Unique actor identifier.
    std::string actor_type;          ///< C++ class name of the actor.
    std::string state;               ///< Current lifecycle state string.
    uint64_t incarnation = 0;        ///< Restart incarnation counter.
    uint64_t messages_processed = 0; ///< Total messages handled.
    uint64_t uptime_ms = 0;          ///< Milliseconds since actor was spawned.
    std::string behavior_name;       ///< Name of the current behavior.
};

/// \brief Mailbox snapshot for CLI introspection.
///
/// Captures a point-in-time view of an actor's mailbox depth, capacity,
/// overflow state, and cumulative statistics.
struct MboxSnapshot {
    uint32_t depth = 0;         ///< Current number of messages in the mailbox.
    uint32_t capacity = 0;      ///< Configured message count capacity.
    uint64_t queued_bytes = 0;  ///< Current queued payload bytes.
    uint64_t byte_capacity = 0; ///< Configured byte capacity.
    uint32_t pressure_ratio_ppm = 0;  ///< Pressure as parts-per-million
                                      ///< (0–1,000,000).
    uint64_t total_enqueued = 0;      ///< Cumulative enqueue count.
    uint64_t total_dequeued = 0;      ///< Cumulative dequeue count.
    uint64_t total_rejected = 0;      ///< Cumulative rejected messages.
    uint64_t total_dropped = 0;       ///< Cumulative dropped messages.
    uint64_t total_dead_letters = 0;  ///< Cumulative dead-lettered messages.
    uint64_t max_depth = 0;           ///< Peak observed depth.
    uint32_t high_priority_depth = 0; ///< Current depth of the high-priority
                                      ///< lane.
    uint32_t system_lane_depth = 0;   ///< Current depth of the system lane.
    /// \brief Per-lane depth snapshot.
    uint32_t lane_depths[8] = {};
    uint8_t num_user_lanes = 1;      ///< Number of configured user lanes.
    uint32_t overflow_depth = 0;     ///< Current overflow queue depth
                                     ///< (SpillToOverflow policy only).
    uint32_t overflow_max_depth = 0; ///< Configured max overflow queue depth (0
                                     ///< = unlimited).
    uint64_t overflow_total_pushed = 0; ///< Cumulative messages spilled to the
                                        ///< overflow queue.
    uint64_t overflow_total_popped = 0; ///< Cumulative messages drained from
                                        ///< overflow back to main.
    uint64_t overflow_total_lost = 0; ///< Cumulative overflow entries silently
                                      ///< evicted.
    std::string pressure_state;       ///< Current pressure state label
                                      ///< (Low/High/Critical).
    std::string overflow_policy;      ///< Configured overflow policy name.
};

/// \brief Lightweight child-actor entry for CLI introspection.
///
/// Named distinctly from protobuf-generated classes to avoid ODR collisions.
struct ChildEntry {
    uint64_t actor_id = 0;  ///< Unique actor identifier.
    std::string actor_type; ///< C++ class name of the child actor.
    std::string state;      ///< Current lifecycle state string.
};

} // namespace cli
} // namespace hpactor
