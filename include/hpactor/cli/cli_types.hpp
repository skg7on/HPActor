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

// Lightweight introspection structs (no protobuf dependency).
// Named distinctly from the protobuf-generated classes in cli_messages.pb.h
// (ActorMetadata, MailboxSnapshot, ChildInfo) to avoid ODR collisions when
// both headers are included in the same translation unit.

struct ActorMeta {
    uint64_t actor_id = 0;
    std::string actor_type;
    std::string state;
    uint64_t incarnation = 0;
    uint64_t messages_processed = 0;
    uint64_t uptime_ms = 0;
    std::string behavior_name;
};

struct MboxSnapshot {
    uint32_t depth = 0;
    uint32_t capacity = 0;
    uint64_t queued_bytes = 0;
    uint64_t byte_capacity = 0;
    uint32_t pressure_ratio_ppm = 0;
    uint64_t total_enqueued = 0;
    uint64_t total_dequeued = 0;
    uint64_t total_rejected = 0;
    uint64_t total_dropped = 0;
    uint64_t total_dead_letters = 0;
    uint64_t max_depth = 0;
    uint32_t high_priority_depth = 0;
    std::string pressure_state;
    std::string overflow_policy;
};

struct ChildEntry {
    uint64_t actor_id = 0;
    std::string actor_type;
    std::string state;
};

} // namespace cli
} // namespace hpactor
