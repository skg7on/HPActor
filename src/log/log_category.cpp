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

#include <hpactor/log/log_category.hpp>

namespace hpactor::log {

[[nodiscard]] const char* to_string(LogCategory category) noexcept {
    switch (category) {
        // NOLINTBEGIN(cppcoreguidelines-macro-usage)
#define HPACTOR_LOG_CATEGORY_TO_STRING(name, str)                              \
    case LogCategory::name:                                                    \
        return str;
        HPACTOR_LOG_CATEGORIES(HPACTOR_LOG_CATEGORY_TO_STRING)
#undef HPACTOR_LOG_CATEGORY_TO_STRING
        // NOLINTEND(cppcoreguidelines-macro-usage)
        case LogCategory::kCount:
            return "count";
    }
    return "unknown";
}

[[nodiscard]] const char* to_string(LogEventId id) noexcept {
    switch (id) {
        // NOLINTBEGIN(cppcoreguidelines-macro-usage)
#define HPACTOR_LOG_EVENT_TO_STRING(name, value, str)                          \
    case LogEventId::name:                                                     \
        return str;
        HPACTOR_LOG_EVENTS(HPACTOR_LOG_EVENT_TO_STRING)
#undef HPACTOR_LOG_EVENT_TO_STRING
        // NOLINTEND(cppcoreguidelines-macro-usage)
    }
    return "unknown_event";
}

[[nodiscard]] result<LogCategory> parse_category(std::string_view value) noexcept {
    // NOLINTBEGIN(cppcoreguidelines-macro-usage)
#define HPACTOR_LOG_CATEGORY_PARSE(name, str)                                  \
    if ((value) == (str))                                                      \
        return result<LogCategory>::make(LogCategory::name);
    HPACTOR_LOG_CATEGORIES(HPACTOR_LOG_CATEGORY_PARSE)
#undef HPACTOR_LOG_CATEGORY_PARSE
    // NOLINTEND(cppcoreguidelines-macro-usage)
    return result<LogCategory>::make(error(errors::unknown, "unknown log "
                                                            "category"));
}

} // namespace hpactor::log
