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
#include <hpactor/cli/actor/cli_http_server_actor.hpp>
#include <hpactor/cli/config/cli_http_server_config.hpp>

#include <chrono>
#include <thread>
#include <type_traits>

TEST(CliHttpServer, ConstructAndStart) {
    hpactor::Config sys_config;
    sys_config.scheduler_threads = 0;
    hpactor::ActorSystem system(sys_config);

    hpactor::cli::CliHttpServerConfig cfg;
    cfg.http_port = 19090;
    cfg.http_bind_address = "127.0.0.1";

    auto server = system.spawn<hpactor::cli::CliHttpServerActor>(cfg);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    auto* raw = static_cast<hpactor::cli::CliHttpServerActor*>(
        system.get_actor(server.id()).get());
    ASSERT_TRUE(raw != nullptr);
    ASSERT_TRUE(raw->is_system_actor());

    raw->request_shutdown();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

TEST(CliHttpServer, ImplementsHostInterfaces) {
    hpactor::Config sys_config;
    sys_config.scheduler_threads = 0;
    hpactor::ActorSystem system(sys_config);

    hpactor::cli::CliHttpServerConfig cfg;
    auto server = system.spawn<hpactor::cli::CliHttpServerActor>(cfg);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    auto* raw = static_cast<hpactor::cli::CliHttpServerActor*>(
        system.get_actor(server.id()).get());

    auto* sys_host = static_cast<hpactor::cli::ISystemCliHost*>(raw);
    auto* life_host = static_cast<hpactor::cli::ILifecycleCliHost*>(raw);
    ASSERT_TRUE(sys_host != nullptr);
    ASSERT_TRUE(life_host != nullptr);

    // CliHttpServerActor implements ICliCommandHost for REST API actor
    // operations. Verify at compile time (dynamic_cast unavailable with
    // -fno-rtti).
    auto* cmd_host = static_cast<hpactor::cli::ICliCommandHost*>(raw);
    ASSERT_TRUE(cmd_host != nullptr);
    static_assert(
        std::is_base_of_v<hpactor::cli::ICliCommandHost, hpactor::cli::CliHttpServerActor>,
        "CliHttpServerActor must expose ICliCommandHost for REST API actor operations");

    raw->request_shutdown();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}
