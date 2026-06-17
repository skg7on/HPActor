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

#include "foreground_runner.hpp"

#include <hpactor/cli/cli_config.hpp>
#include <hpactor/cli/cli_legacy_server_actor.hpp>
#include <hpactor/cli/cli_legacy_server_config.hpp>
#include <hpactor/cli/cli_local_actor.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/process/process_manager.hpp>

#include <chrono>
#include <iostream>
#include <thread>

namespace hpactor::apps::hpactor_demo {

int run_foreground(ActorSystem& system, const ForegroundConfig& cfg) {
    // Spawn CliServerActor for remote hpactor-cli access
    cli::CliLegacyServerConfig server_cfg;
    server_cfg.uds_listen_path =
        cfg.uds_path.empty() ? "/tmp/hpactor/hpactor.sock" : cfg.uds_path;
    server_cfg.max_sessions = 16;
    server_cfg.default_format = "pretty";
    server_cfg.page_size = 20;

    auto cli_server = system.spawn<cli::CliLegacyServerActor>(server_cfg);

    // Notify ready
    process::ProcessManager::notify_ready();

    // Print banner (CliActor owns stdout but banner printed before CLI loop)
    std::cout << "\n[hpactor_demo foreground mode — type /help for commands, "
                 "/quit to exit]\n"
              << "[CliLegacyServerActor listening on "
              << server_cfg.uds_listen_path << "]\n"
              << std::endl;

    // Block until CliActor exits (/quit or EOF)
    while (system.cli_actor() && system.cli_actor()->is_running()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    // Shutdown the CLI server
    auto* server_raw = std::static_pointer_cast<cli::CliLegacyServerActor>(
                           system.get_actor(cli_server.id()))
                           .get();
    if (server_raw) {
        server_raw->request_shutdown();
    }

    std::cout << "\nInitiating graceful shutdown..." << std::endl;
    auto shutdown_result = system.shutdown();
    if (shutdown_result.has_value()) {
        std::cout << "Shutdown complete — all actors drained." << std::endl;
    } else {
        std::cout << "Shutdown timed out — forcing exit." << std::endl;
    }

    std::cout << "=== Demo Complete ===" << std::endl;
    return 0;
}

} // namespace hpactor::apps::hpactor_demo
