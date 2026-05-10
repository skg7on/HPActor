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

#include <hpactor/log/log_level.hpp>

namespace hpactor::log {

[[nodiscard]] const char* to_string(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::kCritical:
            return "critical";
        case LogLevel::kError:
            return "error";
        case LogLevel::kWarning:
            return "warning";
        case LogLevel::kInfo:
            return "info";
        case LogLevel::kDebug:
            return "debug";
        case LogLevel::kTrace:
            return "trace";
        case LogLevel::kOff:
            return "off";
    }
    return "unknown";
}

[[nodiscard]] result<LogLevel> parse_level(std::string_view value) noexcept {
    if (value == "critical")
        return result<LogLevel>::make(LogLevel::kCritical);
    if (value == "error")
        return result<LogLevel>::make(LogLevel::kError);
    if (value == "warning")
        return result<LogLevel>::make(LogLevel::kWarning);
    if (value == "info")
        return result<LogLevel>::make(LogLevel::kInfo);
    if (value == "debug")
        return result<LogLevel>::make(LogLevel::kDebug);
    if (value == "trace")
        return result<LogLevel>::make(LogLevel::kTrace);
    if (value == "off")
        return result<LogLevel>::make(LogLevel::kOff);
    return result<LogLevel>::make(error(errors::unknown, "unknown log level"));
}

} // namespace hpactor::log
