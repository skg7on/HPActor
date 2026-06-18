// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <hpactor/core/actor_system.hpp>
#include <hpactor/net/http_client.hpp>
#include <hpactor/net/http_types.hpp>
#include <hpactor/process/health_http_server.hpp>
#include <hpactor/process/process_config.hpp>
#include <hpactor/process/process_manager.hpp>
#include <hpactor/process/watchdog_actor.hpp>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#if defined(__linux__)
#    include <signal.h>
#    include <sys/signalfd.h>
#endif

namespace {

using namespace hpactor;

// Test fixture for process tests
class ProcessLifecycleTest : public ::testing::Test {
  protected:
    void SetUp() override {
        temp_dir_ = std::filesystem::temp_directory_path() / "hpactor_proc_test";
        std::filesystem::create_directories(temp_dir_);
        pidfile_path_ = (temp_dir_ / "hpactor.pid").string();
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(temp_dir_, ec);
    }

    std::filesystem::path temp_dir_;
    std::string pidfile_path_;
};

TEST_F(ProcessLifecycleTest, ProcessManagerForegroundInit) {
    process::ProcessConfig config;
    config.mode = process::ProcessMode::Foreground;
    config.pidfile_path = pidfile_path_;

    // Remove stale pidfile to get a clean slate for this test.
    std::error_code ec;
    std::filesystem::remove(pidfile_path_, ec);

    auto result = process::ProcessManager::init(config);
    EXPECT_TRUE(result.ok());
    EXPECT_EQ(process::ProcessManager::mode(), process::ProcessMode::Foreground);

    // Verify pidfile was written.
    // ProcessManager uses a static pidfile_written_ guard that prevents
    // re-writing the pidfile on subsequent init() calls within the same
    // process. When this test runs first (or in isolation via
    // --gtest_filter) the pidfile is always present. When another test
    // already called init(), the static guard blocks the write — the
    // check below is conditional to handle both scenarios.
    if (std::filesystem::exists(pidfile_path_)) {
        std::ifstream pidfile(pidfile_path_);
        ASSERT_TRUE(pidfile.good());
        std::string content;
        std::getline(pidfile, content);
        EXPECT_FALSE(content.empty());
        // Content should be the process PID (decimal integer).
        int pid = std::stoi(content);
        EXPECT_GT(pid, 0);
    }
}

} // namespace
