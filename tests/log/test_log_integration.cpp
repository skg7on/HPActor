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

#include <cassert>
#include <chrono>
#include <hpactor/log/log_config.hpp>
#include <hpactor/log/log_event.hpp>
#include <hpactor/log/log_field.hpp>
#include <hpactor/log/log_manager.hpp>
#include <hpactor/log/log_sink.hpp>
#include <hpactor/log/logger.hpp>
#include <string>
#include <thread>

using namespace hpactor;
using namespace hpactor::log;

int main() {
    // Test 1: LogManager creates all components and starts/stops cleanly
    {
        LogConfig cfg;
        cfg.enabled = true;
        cfg.ring_buffer_capacity = 1024;
        cfg.format = LogFormat::kText;

        LogManager mgr(cfg);
        mgr.start();

        // The global logger should be wired and enabled
        // (LogManager configures global_logger() in its constructor)
        auto& gl = global_logger();
        assert(gl.enabled(LogLevel::kInfo, LogCategory::kUser));

        // Give drain thread time to process
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        mgr.stop();
    }

    // Test 2: global_logger is wired after LogManager constructs
    {
        LogConfig cfg;
        cfg.enabled = true;
        cfg.ring_buffer_capacity = 1024;

        LogManager mgr(cfg);
        mgr.start();

        // The global logger should now be enabled
        auto& gl = global_logger();
        assert(gl.enabled(LogLevel::kInfo, LogCategory::kUser));

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        mgr.stop();
    }

    // Test 3: Disabled logging path produces no events
    {
        LogConfig cfg;
        cfg.enabled = false;

        LogManager mgr(cfg);
        // When disabled, events_lost should be 0 (nothing to lose)
        assert(mgr.events_lost() == 0);
    }

    // Test 4: Per-category thresholds filter correctly
    {
        LogConfig cfg;
        cfg.enabled = true;
        cfg.default_level = LogLevel::kInfo;
        cfg.ring_buffer_capacity = 1024;
        // Set mailbox to kOff
        cfg.levels[static_cast<size_t>(LogCategory::kMailbox)] = LogLevel::kOff;

        LogManager mgr(cfg);
        mgr.start();

        auto& gl = global_logger();
        // Mailbox should be disabled
        assert(!gl.enabled(LogLevel::kInfo, LogCategory::kMailbox));
        assert(!gl.enabled(LogLevel::kCritical, LogCategory::kMailbox));
        // But other categories should work
        assert(gl.enabled(LogLevel::kInfo, LogCategory::kActor));

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        mgr.stop();
    }

    // Test 5: Events can be emitted and drained
    {
        LogConfig cfg;
        cfg.enabled = true;
        cfg.ring_buffer_capacity = 1024;

        LogManager mgr(cfg);
        mgr.start();

        HPACTOR_LOG_INFO(LogCategory::kUser, ActorId{0}, 0,
                         "integration test message", field("key", uint64_t(42)));

        // Let drain process
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        mgr.stop();
        // No crash = pass
    }

    // Test 6: Ring buffer overflow recovers
    {
        LogConfig cfg;
        cfg.enabled = true;
        cfg.ring_buffer_capacity = 8; // tiny buffer to force overflow

        LogManager mgr(cfg);
        mgr.start();

        // Emit many events to overflow the buffer
        // Use kInfo since default_level is kInfo; kTrace would be
        // filtered by the Logger before reaching the ring buffer.
        for (int i = 0; i < 10000; i++) {
            HPACTOR_LOG_INFO(LogCategory::kUser, ActorId{0}, 0,
                             "overflow test message");
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        assert(mgr.events_lost() > 0); // some were dropped
        mgr.stop();
    }

    return 0;
}
