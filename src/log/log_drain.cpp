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

#include "hpactor/log/log_drain.hpp"

#include <chrono>
#include <thread>

#include "hpactor/log/log_config.hpp"
#include "hpactor/log/log_event.hpp"
#include "hpactor/log/log_formatter.hpp"
#include "hpactor/log/log_level.hpp"
#include "hpactor/log/log_ring_buffer.hpp"
#include "hpactor/log/log_sink.hpp"

namespace hpactor::log {

LogDrain::LogDrain(LogRingBuffer& buffer, ILogFormatter& formatter,
                   std::vector<std::unique_ptr<ILogSink>> sinks,
                   const LogConfig& config) noexcept
    : buffer_(buffer), formatter_(formatter), sinks_(std::move(sinks)),
      config_(config) {}

LogDrain::~LogDrain() {
    stop();
}

void LogDrain::start() {
    running_.store(true, std::memory_order_release);
    thread_ = std::thread(&LogDrain::run, this);
}

void LogDrain::stop() noexcept {
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false,
                                          std::memory_order_acq_rel)) {
        return; // Wasn't running — nothing to stop.
    }

    if (thread_.joinable()) {
        thread_.join();
    }

    // Final drain of any remaining events that arrived during shutdown.
    std::string buf;
    buf.reserve(4096);
    buffer_.drain([&](const LogEvent& event) {
        buf.clear();
        formatter_.format(event, buf);
        for (auto& sink : sinks_) {
            auto result = sink->write(buf);
            if (!result.has_value()) {
                sink_errors_.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });

    for (auto& sink : sinks_) {
        sink->flush();
    }
}

void LogDrain::nudge() noexcept {
    // No-op. The drain thread polls every 100 ms, so a wake-up signal is not
    // needed for this simplified initial implementation.
}

void LogDrain::run() {
    std::string buf;
    buf.reserve(4096);

    while (running_.load(std::memory_order_relaxed)) {
        buffer_.drain([&](const LogEvent& event) {
            buf.clear();
            formatter_.format(event, buf);
            for (auto& sink : sinks_) {
                auto result = sink->write(buf);
                if (!result.has_value()) {
                    sink_errors_.fetch_add(1, std::memory_order_relaxed);
                }
            }
            if (static_cast<uint8_t>(event.level) <=
                static_cast<uint8_t>(config_.flush_on_level)) {
                for (auto& sink : sinks_) {
                    sink->flush();
                }
            }
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

} // namespace hpactor::log
