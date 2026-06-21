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

#include <hpactor/actor/actor_system.hpp>
#include <hpactor/process/process_config.hpp>
#include <hpactor/process/process_manager.hpp>

using namespace hpactor::process;

TEST(DaemonIntegrationTest, ProcessManagerInitForeground) {
    ProcessConfig cfg;
    cfg.mode = ProcessMode::Foreground;
    auto result = ProcessManager::init(cfg);
    EXPECT_TRUE(result.ok());
    EXPECT_EQ(ProcessManager::mode(), ProcessMode::Foreground);
}

TEST(DaemonIntegrationTest, ProcessManagerInitSystemd) {
    ProcessConfig cfg;
    cfg.mode = ProcessMode::Systemd;
    cfg.watchdog_interval = std::chrono::milliseconds(5000);
    auto result = ProcessManager::init(cfg);
    EXPECT_TRUE(result.ok());
    EXPECT_EQ(ProcessManager::mode(), ProcessMode::Systemd);
}

TEST(DaemonIntegrationTest, NotifyMethodsDoNotCrashInForeground) {
    ProcessConfig cfg;
    cfg.mode = ProcessMode::Foreground;
    ProcessManager::init(cfg);

    // All notify methods should be no-ops in foreground mode
    ProcessManager::notify_ready();
    ProcessManager::notify_watchdog();
    ProcessManager::notify_status("test");
    ProcessManager::notify_stopping();
    ProcessManager::notify_stopped();
    SUCCEED();
}

TEST(DaemonIntegrationTest, SignalHandlersInstall) {
    ProcessConfig cfg;
    ProcessManager::init(cfg);

    int terminate_count = 0;
    int reload_count = 0;
    auto result = ProcessManager::install_signal_handlers(
        [&]() { terminate_count++; }, [&]() { reload_count++; });
    EXPECT_TRUE(result.ok());
}

// When the process is running as a systemd service or daemon, the stdin-based
// CliActor must NOT be spawned — there is no terminal attached.  CLI access in
// those modes goes through CliLegacyServerActor / CliProtoServerActor via
// UDS/TCP sockets instead.
TEST(DaemonIntegrationTest, ActorSystemSkipsCliActorInSystemdMode) {
    hpactor::Config sys_config;
    sys_config.scheduler_threads = 0;
    sys_config.cli.enabled = true;
    sys_config.process.mode = ProcessMode::Systemd;

    hpactor::ActorSystem system(sys_config);

    // CliActor (stdin-based) must NOT be spawned in non-foreground modes.
    EXPECT_EQ(system.cli_actor(), nullptr);
}
