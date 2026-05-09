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
#include <span>

namespace hpactor::log {

inline constexpr uint8_t kMaxLogFields = 4;

enum class LogFieldType : uint8_t {
    kInt64,
    kUInt64,
    kDouble,
    kBool,
    kStringLiteral,
    kPointer,
};

struct LogField {
    const char* name;
    LogFieldType type;
    union {
        int64_t i64;
        uint64_t u64;
        double f64;
        bool boolean;
        const char* str;
        const void* ptr;
    } value;
};

inline LogField field(const char* name, int64_t value) noexcept {
    LogField f{};
    f.name = name;
    f.type = LogFieldType::kInt64;
    f.value.i64 = value;
    return f;
}

inline LogField field(const char* name, uint64_t value) noexcept {
    LogField f{};
    f.name = name;
    f.type = LogFieldType::kUInt64;
    f.value.u64 = value;
    return f;
}

inline LogField field(const char* name, double value) noexcept {
    LogField f{};
    f.name = name;
    f.type = LogFieldType::kDouble;
    f.value.f64 = value;
    return f;
}

inline LogField field(const char* name, bool value) noexcept {
    LogField f{};
    f.name = name;
    f.type = LogFieldType::kBool;
    f.value.boolean = value;
    return f;
}

inline LogField field_lit(const char* name, const char* value) noexcept {
    LogField f{};
    f.name = name;
    f.type = LogFieldType::kStringLiteral;
    f.value.str = value;
    return f;
}

inline LogField field_ptr(const char* name, const void* value) noexcept {
    LogField f{};
    f.name = name;
    f.type = LogFieldType::kPointer;
    f.value.ptr = value;
    return f;
}

} // namespace hpactor::log
