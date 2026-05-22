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

#include <gtest/gtest.h>
#include <hpactor/log/log_config.hpp>

using namespace hpactor::log;

TEST(LogConfigTest, Defaults) {
    LogConfig cfg{};
    EXPECT_TRUE(cfg.enabled);
    EXPECT_EQ(cfg.default_level, LogLevel::kInfo);
    EXPECT_EQ(cfg.format, LogFormat::kJson);
    EXPECT_EQ(cfg.ring_buffer_capacity, 65536u);
    EXPECT_EQ(cfg.flush_on_level, LogLevel::kError);
}

TEST(LogConfigTest, CategoryLevelOverrides) {
    LogConfig cfg{};
    cfg.default_level = LogLevel::kInfo;
    cfg.levels[static_cast<size_t>(LogCategory::kMailbox)] = LogLevel::kWarning;
    cfg.levels[static_cast<size_t>(LogCategory::kMemory)] = LogLevel::kWarning;
    cfg.levels[static_cast<size_t>(LogCategory::kNetwork)] = LogLevel::kWarning;

    EXPECT_EQ(cfg.levels[static_cast<size_t>(LogCategory::kMailbox)],
              LogLevel::kWarning);
    EXPECT_EQ(cfg.levels[static_cast<size_t>(LogCategory::kActor)],
              LogLevel::kCritical);
}

TEST(LogConfigTest, Disabled) {
    LogConfig cfg{};
    cfg.enabled = false;
    EXPECT_FALSE(cfg.enabled);
}
