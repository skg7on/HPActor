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
#include <type_traits>

#include <hpactor/log/log_category.hpp>
#include <hpactor/log/log_field.hpp>
#include <hpactor/log/log_level.hpp>
#include <hpactor/types/types.hpp>

namespace hpactor::log {

struct LogEvent {
    uint64_t timestamp_ns; // Unix epoch ns from system_clock
    LogLevel level;
    LogCategory category;
    ActorId actor_id;   // 0 = no actor context
    uint64_t trace_id;  // from TraceContext when available
    uint64_t span_id;   // from TraceContext when available
    uint32_t worker_id; // scheduler worker, UINT32_MAX if unknown
    uint32_t type_tag;  // TypedMessage tag when available
    uint32_t event_id;  // stable numeric LogEventId
    uint32_t line;
    const char* file;
    const char* message;
    LogField fields[kMaxLogFields];
    uint8_t field_count;
};

static_assert(std::is_trivially_copyable_v<LogEvent>, "LogEvent must be "
                                                      "trivially copyable for "
                                                      "ring buffer");

} // namespace hpactor::log
