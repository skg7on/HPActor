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

#include "hpactor/log/log_manager.hpp"

#include <cassert>

#include "hpactor/log/log_drain.hpp"
#include "hpactor/log/log_formatter.hpp"
#include "hpactor/log/log_ring_buffer.hpp"
#include "hpactor/log/log_sink.hpp"
#include "hpactor/log/logger.hpp"

namespace hpactor::log {

namespace {

void nudge_callback(void* ctx) noexcept {
    static_cast<LogDrain*>(ctx)->nudge();
}

bool is_noisy_category(LogCategory category) noexcept {
    switch (category) {
        case LogCategory::kMailbox:
        case LogCategory::kMemory:
        case LogCategory::kNetwork:
        case LogCategory::kActorState:
        case LogCategory::kScheduler:
            return true;
        default:
            return false;
    }
}

} // namespace

LogManager::LogManager(const LogConfig& config) : config_(config) {
    // 1. Validate ring_buffer_capacity is power of two (LogRingBuffer checks
    // this too, but we assert here for early failure in debug builds).
    assert(config_.ring_buffer_capacity > 0 &&
           (config_.ring_buffer_capacity & (config_.ring_buffer_capacity - 1)) == 0);

    // 2. Build per-category level thresholds.
    for (size_t i = 0; i < static_cast<size_t>(LogCategory::kCount); ++i) {
        auto cat = static_cast<LogCategory>(i);
        LogLevel explicit_level = config_.levels[i];
        if (explicit_level != LogLevel::kCritical) {
            // kCritical (value 0) means "not set" in config_.levels.
            resolved_levels_[i] = explicit_level;
        } else if (is_noisy_category(cat)) {
            resolved_levels_[i] = LogLevel::kWarning;
        } else {
            resolved_levels_[i] = config_.default_level;
        }
    }

    // 3. Create ring buffer.
    ring_buffer_ = std::make_unique<LogRingBuffer>(config_.ring_buffer_capacity);

    // 4. Create formatter.
    switch (config_.format) {
        case LogFormat::kText:
            formatter_ = std::make_unique<TextLogFormatter>();
            break;
        case LogFormat::kJson:
        default:
            formatter_ = std::make_unique<JsonLogFormatter>();
            break;
    }

    // 5. Create sinks. For now only MemorySink exists; stderr, file, and
    //    rotating-file sinks will be added in Tasks 14-16.
    //    When config_.sinks is empty, MemorySink serves as the default.
    //    When non-empty, future tasks will switch on LogSinkKind.
    sinks_.push_back(std::make_unique<MemorySink>());

    // 6. Create drain (moves sinks).
    drain_ = std::make_unique<LogDrain>(*ring_buffer_, *formatter_,
                                        std::move(sinks_), config_);

    // 7. Create logger.
    logger_ = std::make_unique<Logger>();

    // 8. Install as the global logger instance so HPACTOR_LOG_* macros work.
    global_logger().configure(ring_buffer_.get(), &resolved_levels_,
                              config_.flush_on_level, nudge_callback, drain_.get());
}

LogManager::~LogManager() {
    stop();
}

void LogManager::start() {
    drain_->start();
}

void LogManager::stop() noexcept {
    drain_->stop();
}

uint64_t LogManager::events_lost() const noexcept {
    return ring_buffer_->events_lost();
}

uint64_t LogManager::sink_errors() const noexcept {
    return drain_->sink_errors();
}

} // namespace hpactor::log
