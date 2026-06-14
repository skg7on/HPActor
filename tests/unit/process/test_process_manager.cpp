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
#include <hpactor/process/process_config.hpp>

using namespace hpactor::process;

TEST(ProcessModeTest, ParseForeground) {
    EXPECT_EQ(ProcessConfig::parse_mode("foreground"), ProcessMode::Foreground);
    EXPECT_EQ(ProcessConfig::parse_mode("FOREGROUND"), ProcessMode::Foreground);
    EXPECT_EQ(ProcessConfig::parse_mode("Foreground"), ProcessMode::Foreground);
}

TEST(ProcessModeTest, ParseSystemd) {
    EXPECT_EQ(ProcessConfig::parse_mode("systemd"), ProcessMode::Systemd);
    EXPECT_EQ(ProcessConfig::parse_mode("SYSTEMD"), ProcessMode::Systemd);
}

TEST(ProcessModeTest, ParseDaemon) {
    EXPECT_EQ(ProcessConfig::parse_mode("daemon"), ProcessMode::Daemon);
    EXPECT_EQ(ProcessConfig::parse_mode("DAEMON"), ProcessMode::Daemon);
}

TEST(ProcessModeTest, ParseUnknownDefaultsToForeground) {
    EXPECT_EQ(ProcessConfig::parse_mode("bogus"), ProcessMode::Foreground);
    EXPECT_EQ(ProcessConfig::parse_mode(""), ProcessMode::Foreground);
}

TEST(ProcessModeTest, DefaultConfigIsForeground) {
    ProcessConfig cfg;
    EXPECT_EQ(cfg.mode, ProcessMode::Foreground);
    EXPECT_EQ(cfg.watchdog_interval.count(), 0);
    EXPECT_TRUE(cfg.pidfile_path.empty());
}
