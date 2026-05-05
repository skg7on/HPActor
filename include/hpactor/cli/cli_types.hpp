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

struct ActorMetadata {
    uint64_t actor_id = 0;
    std::string actor_type;
    std::string state;
    uint64_t incarnation = 0;
    uint64_t messages_processed = 0;
    uint64_t uptime_ms = 0;
    std::string behavior_name;
};

struct MailboxSnapshot {
    uint32_t depth = 0;
    uint64_t total_enqueued = 0;
    uint64_t total_dequeued = 0;
    uint64_t max_depth = 0;
    uint32_t high_priority_depth = 0;
};

struct ChildInfo {
    uint64_t actor_id = 0;
    std::string actor_type;
    std::string state;
};

}  // namespace cli
}  // namespace hpactor
