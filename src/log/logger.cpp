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

#include <chrono>
#include <hpactor/log/log_ring_buffer.hpp>
#include <hpactor/log/logger.hpp>

namespace hpactor::log {

void Logger::emit(LogEvent event) noexcept {
    if (!buffer_)
        return;

    auto now = std::chrono::system_clock::now();
    event.timestamp_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch())
            .count());

    buffer_->try_push(event);

    // Nudge the drain thread for events at or above flush_on_level
    if (static_cast<uint8_t>(event.level) <= static_cast<uint8_t>(flush_on_level_)) {
        nudge();
    }
}

void Logger::emit(LogLevel level, LogCategory category, ActorId actor_id,
                  uint32_t event_id, const char* message, const LogField* fields,
                  uint8_t field_count, const char* file, uint32_t line) noexcept {
    if (!buffer_)
        return;

    LogEvent evt{};
    evt.level = level;
    evt.category = category;
    evt.actor_id = actor_id;
    evt.event_id = event_id;
    evt.message = message;
    evt.file = file;
    evt.line = line;
    evt.worker_id = UINT32_MAX;
    evt.field_count = (field_count > kMaxLogFields) ? kMaxLogFields : field_count;
    if (field_count > kMaxLogFields) {
        fields_dropped_ += field_count - kMaxLogFields;
    }
    for (uint8_t i = 0; i < evt.field_count; ++i) {
        evt.fields[i] = fields[i];
    }

    auto now = std::chrono::system_clock::now();
    evt.timestamp_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch())
            .count());

    buffer_->try_push(evt);

    if (static_cast<uint8_t>(level) <= static_cast<uint8_t>(flush_on_level_)) {
        nudge();
    }
}

namespace {
Logger g_noop_logger; // buffer_ is null, all operations are no-ops
}

Logger& global_logger() noexcept {
    return g_noop_logger;
}

} // namespace hpactor::log
