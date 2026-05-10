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

#include <array>

#include "hpactor/hpactor_config.hpp"
#include "hpactor/log/log_category.hpp"
#include "hpactor/log/log_event.hpp"
#include "hpactor/log/log_field.hpp"
#include "hpactor/log/log_level.hpp"
#include "hpactor/types/types.hpp"

namespace hpactor::log {

class Logger {
  public:
    Logger() = default;

    // Set by LogManager when logging is active
    void
    configure(class LogRingBuffer* buffer,
              const std::array<LogLevel, static_cast<size_t>(LogCategory::kCount)>* levels,
              LogLevel flush_on_level = LogLevel::kError,
              void (*nudge_fn)(void*) noexcept = nullptr,
              void* nudge_ctx = nullptr) noexcept {
        buffer_ = buffer;
        levels_ = levels;
        flush_on_level_ = flush_on_level;
        nudge_fn_ = nudge_fn;
        nudge_ctx_ = nudge_ctx;
    }

    bool enabled(LogLevel level, LogCategory category) const noexcept {
        if (!buffer_ || !levels_)
            return false;
        auto idx = static_cast<size_t>(category);
        LogLevel threshold = (*levels_)[idx];
        if (threshold == LogLevel::kOff)
            return false;
        return static_cast<uint8_t>(level) <= static_cast<uint8_t>(threshold);
    }

    void nudge() noexcept {
        if (nudge_fn_)
            nudge_fn_(nudge_ctx_);
    }

    void emit(LogEvent event) noexcept;

    void emit(LogLevel level, LogCategory category, ActorId actor_id,
              uint32_t event_id, const char* message, const LogField* fields,
              uint8_t field_count, const char* file, uint32_t line) noexcept;

    uint64_t fields_dropped() const noexcept {
        return fields_dropped_;
    }

  private:
    class LogRingBuffer* buffer_ = nullptr;
    const std::array<LogLevel, static_cast<size_t>(LogCategory::kCount)>* levels_ =
        nullptr;
    LogLevel flush_on_level_ = LogLevel::kError;
    uint64_t fields_dropped_{0};
    void (*nudge_fn_)(void*) noexcept = nullptr;
    void* nudge_ctx_ = nullptr;
};

Logger& global_logger() noexcept;

} // namespace hpactor::log

#if HPACTOR_ENABLE_ACTOR_LOGGING

#    define HPACTOR_LOG_CRITICAL(category, actor_id, event_id, message, ...)   \
        do {                                                                   \
            auto& __hpactor_logger = ::hpactor::log::global_logger();          \
            if (__hpactor_logger.enabled(::hpactor::log::LogLevel::kCritical,  \
                                         category)) {                          \
                ::hpactor::log::LogField __hpactor_fields[] = {__VA_ARGS__};   \
                __hpactor_logger.emit(                                         \
                    ::hpactor::log::LogLevel::kCritical, category, actor_id,   \
                    event_id, message, __hpactor_fields,                       \
                    static_cast<uint8_t>(sizeof(__hpactor_fields) /            \
                                         sizeof(__hpactor_fields[0])),         \
                    __FILE__, __LINE__);                                       \
            }                                                                  \
        } while (0)

#    define HPACTOR_LOG_ERROR(category, actor_id, event_id, message, ...)      \
        do {                                                                   \
            auto& __hpactor_logger = ::hpactor::log::global_logger();          \
            if (__hpactor_logger.enabled(::hpactor::log::LogLevel::kError,     \
                                         category)) {                          \
                ::hpactor::log::LogField __hpactor_fields[] = {__VA_ARGS__};   \
                __hpactor_logger.emit(                                         \
                    ::hpactor::log::LogLevel::kError, category, actor_id,      \
                    event_id, message, __hpactor_fields,                       \
                    static_cast<uint8_t>(sizeof(__hpactor_fields) /            \
                                         sizeof(__hpactor_fields[0])),         \
                    __FILE__, __LINE__);                                       \
            }                                                                  \
        } while (0)

#    define HPACTOR_LOG_WARNING(category, actor_id, event_id, message, ...)    \
        do {                                                                   \
            auto& __hpactor_logger = ::hpactor::log::global_logger();          \
            if (__hpactor_logger.enabled(::hpactor::log::LogLevel::kWarning,   \
                                         category)) {                          \
                ::hpactor::log::LogField __hpactor_fields[] = {__VA_ARGS__};   \
                __hpactor_logger.emit(                                         \
                    ::hpactor::log::LogLevel::kWarning, category, actor_id,    \
                    event_id, message, __hpactor_fields,                       \
                    static_cast<uint8_t>(sizeof(__hpactor_fields) /            \
                                         sizeof(__hpactor_fields[0])),         \
                    __FILE__, __LINE__);                                       \
            }                                                                  \
        } while (0)

#    define HPACTOR_LOG_INFO(category, actor_id, event_id, message, ...)       \
        do {                                                                   \
            auto& __hpactor_logger = ::hpactor::log::global_logger();          \
            if (__hpactor_logger.enabled(::hpactor::log::LogLevel::kInfo,      \
                                         category)) {                          \
                ::hpactor::log::LogField __hpactor_fields[] = {__VA_ARGS__};   \
                __hpactor_logger.emit(                                         \
                    ::hpactor::log::LogLevel::kInfo, category, actor_id,       \
                    event_id, message, __hpactor_fields,                       \
                    static_cast<uint8_t>(sizeof(__hpactor_fields) /            \
                                         sizeof(__hpactor_fields[0])),         \
                    __FILE__, __LINE__);                                       \
            }                                                                  \
        } while (0)

#    define HPACTOR_LOG_DEBUG(category, actor_id, event_id, message, ...)      \
        do {                                                                   \
            auto& __hpactor_logger = ::hpactor::log::global_logger();          \
            if (__hpactor_logger.enabled(::hpactor::log::LogLevel::kDebug,     \
                                         category)) {                          \
                ::hpactor::log::LogField __hpactor_fields[] = {__VA_ARGS__};   \
                __hpactor_logger.emit(                                         \
                    ::hpactor::log::LogLevel::kDebug, category, actor_id,      \
                    event_id, message, __hpactor_fields,                       \
                    static_cast<uint8_t>(sizeof(__hpactor_fields) /            \
                                         sizeof(__hpactor_fields[0])),         \
                    __FILE__, __LINE__);                                       \
            }                                                                  \
        } while (0)

#    define HPACTOR_LOG_TRACE(category, actor_id, event_id, message, ...)      \
        do {                                                                   \
            auto& __hpactor_logger = ::hpactor::log::global_logger();          \
            if (__hpactor_logger.enabled(::hpactor::log::LogLevel::kTrace,     \
                                         category)) {                          \
                ::hpactor::log::LogField __hpactor_fields[] = {__VA_ARGS__};   \
                __hpactor_logger.emit(                                         \
                    ::hpactor::log::LogLevel::kTrace, category, actor_id,      \
                    event_id, message, __hpactor_fields,                       \
                    static_cast<uint8_t>(sizeof(__hpactor_fields) /            \
                                         sizeof(__hpactor_fields[0])),         \
                    __FILE__, __LINE__);                                       \
            }                                                                  \
        } while (0)

#else // HPACTOR_ENABLE_ACTOR_LOGGING disabled

#    define HPACTOR_LOG_CRITICAL(category, actor_id, event_id, message, ...)   \
        ((void)0)
#    define HPACTOR_LOG_ERROR(category, actor_id, event_id, message, ...)      \
        ((void)0)
#    define HPACTOR_LOG_WARNING(category, actor_id, event_id, message, ...)    \
        ((void)0)
#    define HPACTOR_LOG_INFO(category, actor_id, event_id, message, ...)       \
        ((void)0)
#    define HPACTOR_LOG_DEBUG(category, actor_id, event_id, message, ...)      \
        ((void)0)
#    define HPACTOR_LOG_TRACE(category, actor_id, event_id, message, ...)      \
        ((void)0)

#endif // HPACTOR_ENABLE_ACTOR_LOGGING
