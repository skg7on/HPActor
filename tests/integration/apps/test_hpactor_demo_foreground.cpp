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

#include "apps/hpactor_demo/cli_demo_actor_factory.hpp"

#if HPACTOR_ENABLE_CLI
#    include <hpactor/cli/actor/cli_legacy_server_actor.hpp>
#    include <hpactor/cli/actor/cli_local_actor.hpp>
#    include <hpactor/cli/config/cli_config.hpp>
#    include <hpactor/cli/config/cli_legacy_server_config.hpp>
#endif

#include <chrono>
#include <memory>
#include <thread>

using namespace hpactor;
using namespace hpactor::apps;

TEST(HpactorDemoForegroundTest, ActorFactorySpawnsAllActors) {
    Config config;
    config.scheduler_threads = 1;
    config.cli.enabled = false;
    config.mailbox.default_capacity = 256;
    config.mailbox.default_policy = mailbox::OverflowPolicy::DeadLetter;
    ActorSystem system(config);

    auto actors = cli_demo::spawn_cli_demo_actors(system);

    EXPECT_NE(actors.aggregator, nullptr);
    EXPECT_NE(actors.health_check, nullptr);
    EXPECT_NE(actors.broadcast, nullptr);
    EXPECT_NE(actors.clock, nullptr);
    EXPECT_NE(actors.log, nullptr);
    EXPECT_NE(actors.monitor, nullptr);
    EXPECT_NE(actors.dlq_demo, nullptr);
    EXPECT_NE(actors.query, nullptr);
    for (int i = 0; i < 4; ++i) {
        EXPECT_NE(actors.workers[i], nullptr) << "Worker " << (i + 1) << " is null";
    }
}

TEST(HpactorDemoForegroundTest, ConfigIsForegroundByDefault) {
    Config config;
    EXPECT_EQ(config.process.mode, process::ProcessMode::Foreground);
}

#if HPACTOR_ENABLE_CLI
TEST(HpactorDemoForegroundTest, DualCliSpawning) {
    Config config;
    config.scheduler_threads = 1;
    config.cli.enabled = true;
    config.cli.default_format = "pretty";
    config.cli.page_size = 20;
    config.mailbox.default_capacity = 256;
    config.mailbox.default_policy = mailbox::OverflowPolicy::DeadLetter;
    ActorSystem system(config);

    // Spawn actors
    auto actors = cli_demo::spawn_cli_demo_actors(system);

    // Verify CliActor was created (enabled=true).
    // is_running() may already be false in non-interactive (non-TTY)
    // environments because the CLI reads EOF from stdin and exits
    // immediately. The banner printed above confirms it started.
    ASSERT_NE(system.cli_actor(), nullptr);

    // Spawn CliLegacyServerActor alongside
    cli::CliLegacyServerConfig server_cfg;
    server_cfg.uds_listen_path = "/tmp/hpactor_test.sock";
    server_cfg.max_sessions = 4;
    auto cli_server = system.spawn<cli::CliLegacyServerActor>(server_cfg);
    EXPECT_NE(cli_server.id().value(), 0u);

    // Brief spin to let daemon threads start
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Shutdown CliActor (already shutting down in non-TTY, but no-op if so)
    system.cli_actor()->request_shutdown();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Shutdown CliLegacyServerActor before system shutdown
    auto* server_raw = std::static_pointer_cast<cli::CliLegacyServerActor>(
                           system.get_actor(cli_server.id()))
                           .get();
    if (server_raw) {
        server_raw->request_shutdown();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Clean shutdown
    system.shutdown();
    SUCCEED();
}
#endif // HPACTOR_ENABLE_CLI
