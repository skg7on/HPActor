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

} // namespace
