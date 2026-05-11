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
#include <hpactor/log/detail/log_macros.hpp>
#include <hpactor/types/types.hpp>
#include <string_view>

namespace hpactor::log {

enum class LogCategory : uint16_t {
#define HPACTOR_LOG_CATEGORY_ENUM(name, str) name,
    HPACTOR_LOG_CATEGORIES(HPACTOR_LOG_CATEGORY_ENUM)
#undef HPACTOR_LOG_CATEGORY_ENUM
        kCount, // sentinel, not a valid emit category
};

// Stable numeric IDs for common framework events.
// Ranges: 1000-1099 actor, 1100-1199 mailbox, 1200-1299 memory,
//         1300-1399 registrar/discovery, 1400-1499 network, 1500-1599 scheduler
enum class LogEventId : uint32_t {
#define HPACTOR_LOG_EVENT_ENUM(name, value, str) name = value,
    HPACTOR_LOG_EVENTS(HPACTOR_LOG_EVENT_ENUM)
#undef HPACTOR_LOG_EVENT_ENUM
};

[[nodiscard]] const char* to_string(LogCategory category) noexcept;
[[nodiscard]] const char* to_string(LogEventId id) noexcept;
[[nodiscard]] result<LogCategory> parse_category(std::string_view value) noexcept;

} // namespace hpactor::log
