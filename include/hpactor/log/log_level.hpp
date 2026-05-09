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

const char* to_string(LogLevel level) noexcept;
result<LogLevel> parse_level(std::string_view value) noexcept;

} // namespace hpactor::log
