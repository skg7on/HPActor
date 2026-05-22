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

#include <cstdio>
#include <fstream>
#include <gtest/gtest.h>
#include <hpactor/log/log_config.hpp>
#include <hpactor/log/log_sink.hpp>
#include <string>
#include <sys/stat.h>

using namespace hpactor;

static std::string read_file(const std::string& path) {
    std::ifstream f(path);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

static bool file_exists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

TEST(LogSinksTest, StderrWrite) {
    auto sink = log::make_stderr_sink();
    EXPECT_NE(sink, nullptr);
    EXPECT_TRUE(sink->write("hello_stderr_test").has_value());
}

TEST(LogSinksTest, StderrFlush) {
    auto sink = log::make_stderr_sink();
    EXPECT_TRUE(sink->flush().has_value());
}

TEST(LogSinksTest, FileWrite) {
    const char* path = "/tmp/hpactor_test_file_sink.log";
    std::remove(path);
    auto sink = log::make_file_sink(path);
    EXPECT_NE(sink, nullptr);
    EXPECT_TRUE(sink->write("hello_file").has_value());
    EXPECT_TRUE(sink->write("world_file").has_value());
    EXPECT_TRUE(sink->flush().has_value());
    std::string content = read_file(path);
    EXPECT_NE(content.find("hello_file"), std::string::npos);
    EXPECT_NE(content.find("world_file"), std::string::npos);
    std::remove(path);
}

TEST(LogSinksTest, FileFlush) {
    const char* path = "/tmp/hpactor_test_file_flush.log";
    std::remove(path);
    auto sink = log::make_file_sink(path);
    sink->write("flush_test");
    EXPECT_TRUE(sink->flush().has_value());
    EXPECT_NE(read_file(path).find("flush_test"), std::string::npos);
    std::remove(path);
}

TEST(LogSinksTest, FileFactory) {
    EXPECT_NE(log::make_file_sink("/tmp/test.log"), nullptr);
}

TEST(LogSinksTest, RotatingWriteBelowThreshold) {
    const char* path = "/tmp/hpactor_test_rotating.log";
    std::remove(path);
    log::RotatingFileConfig cfg;
    cfg.path = path;
    cfg.max_bytes = 1024;
    cfg.max_files = 3;
    auto sink = log::make_rotating_file_sink(cfg);
    EXPECT_NE(sink, nullptr);
    EXPECT_TRUE(sink->write("short_line").has_value());
    EXPECT_TRUE(sink->flush().has_value());
    EXPECT_NE(read_file(path).find("short_line"), std::string::npos);
    std::remove(path);
}

TEST(LogSinksTest, RotatingTriggersRotation) {
    const char* path = "/tmp/hpactor_test_rotate_trigger.log";
    std::remove(path);
    std::remove((std::string(path) + ".1").c_str());
    std::remove((std::string(path) + ".2").c_str());
    log::RotatingFileConfig cfg;
    cfg.path = path;
    cfg.max_bytes = 10;
    cfg.max_files = 2;
    auto sink = log::make_rotating_file_sink(cfg);
    EXPECT_NE(sink, nullptr);
    EXPECT_TRUE(sink->write("hello_world_long_enough").has_value());
    EXPECT_TRUE(sink->write("short").has_value());
    EXPECT_TRUE(sink->flush().has_value());
    std::string rotated_path = std::string(path) + ".1";
    EXPECT_TRUE(file_exists(rotated_path));
    std::string content = read_file(rotated_path);
    EXPECT_NE(content.find("hello_world_long_enough"), std::string::npos);
    std::string current_content = read_file(path);
    EXPECT_NE(current_content.find("short"), std::string::npos);
    std::remove(path);
    std::remove(rotated_path.c_str());
}

TEST(LogSinksTest, RotatingFlush) {
    const char* path = "/tmp/hpactor_test_rotating_flush.log";
    std::remove(path);
    log::RotatingFileConfig cfg;
    cfg.path = path;
    cfg.max_bytes = 1024;
    cfg.max_files = 2;
    auto sink = log::make_rotating_file_sink(cfg);
    sink->write("flush_me");
    EXPECT_TRUE(sink->flush().has_value());
    EXPECT_NE(read_file(path).find("flush_me"), std::string::npos);
    std::remove(path);
}

TEST(LogSinksTest, RotatingFactory) {
    log::RotatingFileConfig cfg;
    cfg.path = "/tmp/test_rot.log";
    cfg.max_bytes = 1024;
    cfg.max_files = 2;
    EXPECT_NE(log::make_rotating_file_sink(cfg), nullptr);
}
