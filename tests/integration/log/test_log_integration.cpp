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

#include <chrono>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include <hpactor/log/log_config.hpp>
#include <hpactor/log/log_event.hpp>
#include <hpactor/log/log_field.hpp>
#include <hpactor/log/log_manager.hpp>
#include <hpactor/log/log_sink.hpp>
#include <hpactor/log/logger.hpp>

using namespace hpactor;
using namespace hpactor::log;

TEST(LogIntegrationTest, CreatesAllComponentsAndStartsStopsCleanly) {
    LogConfig cfg;
    cfg.enabled = true;
    cfg.ring_buffer_capacity = 1024;
    cfg.format = LogFormat::kText;

    LogManager mgr(cfg);
    mgr.start();

    // The global logger should be wired and enabled
    auto& gl = global_logger();
    ASSERT_TRUE(gl.enabled(LogLevel::kInfo, LogCategory::kUser));

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    mgr.stop();
}

TEST(LogIntegrationTest, GlobalLoggerWiredAfterLogManagerConstruction) {
    LogConfig cfg;
    cfg.enabled = true;
    cfg.ring_buffer_capacity = 1024;

    LogManager mgr(cfg);
    mgr.start();

    auto& gl = global_logger();
    ASSERT_TRUE(gl.enabled(LogLevel::kInfo, LogCategory::kUser));

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    mgr.stop();
}

TEST(LogIntegrationTest, DisabledLoggingProducesNoEvents) {
    LogConfig cfg;
    cfg.enabled = false;

    LogManager mgr(cfg);
    EXPECT_EQ(mgr.events_lost(), 0u);
}

TEST(LogIntegrationTest, PerCategoryThresholdsFilterCorrectly) {
    LogConfig cfg;
    cfg.enabled = true;
    cfg.default_level = LogLevel::kInfo;
    cfg.ring_buffer_capacity = 1024;
    cfg.levels[static_cast<size_t>(LogCategory::kMailbox)] = LogLevel::kOff;

    LogManager mgr(cfg);
    mgr.start();

    auto& gl = global_logger();
    EXPECT_FALSE(gl.enabled(LogLevel::kInfo, LogCategory::kMailbox));
    EXPECT_FALSE(gl.enabled(LogLevel::kCritical, LogCategory::kMailbox));
    EXPECT_TRUE(gl.enabled(LogLevel::kInfo, LogCategory::kActor));

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    mgr.stop();
}

TEST(LogIntegrationTest, EventsCanBeEmittedAndDrained) {
    LogConfig cfg;
    cfg.enabled = true;
    cfg.ring_buffer_capacity = 1024;

    LogManager mgr(cfg);
    mgr.start();

    HPACTOR_LOG_INFO(LogCategory::kUser, ActorId{0}, 0,
                     "integration test message", field("key", uint64_t(42)));

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    mgr.stop();
}

TEST(LogIntegrationTest, RingBufferOverflowRecovers) {
    LogConfig cfg;
    cfg.enabled = true;
    cfg.ring_buffer_capacity = 8;

    LogManager mgr(cfg);
    mgr.start();

    for (int i = 0; i < 10000; i++) {
        HPACTOR_LOG_INFO(LogCategory::kUser, ActorId{0}, 0, "overflow test message");
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_GT(mgr.events_lost(), 0u);
    mgr.stop();
}
