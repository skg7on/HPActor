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
#include <hpactor/types/types.hpp>
#include <string_view>

namespace hpactor::log {

enum class LogLevel : uint8_t {
    kCritical = 0,
    kError = 1,
    kWarning = 2,
    kInfo = 3,
    kDebug = 4,
    kTrace = 5,
    kOff = 6,
};

[[nodiscard]] const char* to_string(LogLevel level) noexcept;
[[nodiscard]] result<LogLevel> parse_level(std::string_view value) noexcept;

} // namespace hpactor::log
