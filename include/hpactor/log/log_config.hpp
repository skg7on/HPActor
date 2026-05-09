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

#include "hpactor/log/log_category.hpp"
#include "hpactor/log/log_level.hpp"
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace hpactor::log {

enum class LogFormat : uint8_t {
    kText,
    kJson,
};

enum class DropPolicy : uint8_t {
    kDropNewest,
};

enum class LogSinkKind : uint8_t {
    kStderr,
    kFile,
    kRotatingFile,
};

struct RotatingFileConfig {
    std::string path;
    uint64_t max_bytes = 104857600; // 100 MiB
    uint32_t max_files = 5;
};

struct LogConfig {
    bool enabled = true;
    LogLevel default_level = LogLevel::kInfo;
    std::array<LogLevel, static_cast<size_t>(LogCategory::kCount)> levels{};
    LogFormat format = LogFormat::kJson;
    DropPolicy drop_policy = DropPolicy::kDropNewest;
    uint32_t ring_buffer_capacity = 65536;
    LogLevel flush_on_level = LogLevel::kError;
    std::vector<LogSinkKind> sinks;
    std::string file_path;
    RotatingFileConfig rotating_file;
};

} // namespace hpactor::log
