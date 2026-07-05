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

#include <hpactor/actor/system/actor_system.hpp>
#include <hpactor/cli/actor/cli_local_actor.hpp>
#include <hpactor/cli/actor/cli_proto_server_actor.hpp>
#include <hpactor/cli/config/cli_config.hpp>
#include <hpactor/cli/config/cli_proto_server_config.hpp>
#include <hpactor/process/process_manager.hpp>

#include <chrono>
#include <iostream>
#include <thread>

namespace hpactor::apps::hpactor_demo {

int run_foreground(ActorSystem& system, const ForegroundConfig& cfg) {
    // Spawn CliProtoServerActor for remote hpactor-cli access.
    // CliProtoServerActor speaks the HPAC Frame protobuf wire protocol used by
    // CliClientActor (hpactor-cli tool).  CliLegacyServerActor used a raw text
    // protocol which was wire-incompatible with hpactor-cli.
    // uds_path is always populated by main.cpp (either user-provided or
    // platform-aware default); no fallback needed here.
    cli::CliProtoServerConfig server_cfg;
    server_cfg.uds_listen_path = cfg.uds_path;
    server_cfg.tcp_listen_port = cfg.tcp_port;
    server_cfg.max_sessions = 16;
    server_cfg.default_format = "pretty";
    server_cfg.page_size = 20;

    auto cli_server = system.spawn<cli::CliProtoServerActor>(server_cfg);

    // Notify ready (no-op in foreground mode, kept for consistency)
    process::ProcessManager::notify_ready();

    // Status messages are printed in main.cpp before ActorSystem construction
    // to avoid racing with the CliActor daemon thread's linenoise raw terminal.

    // Block until CliActor exits (/quit or EOF).
    //
    // TODO: Replace polling with a std::promise/future signaled by CliActor
    // when its daemon loop exits.  This requires adding a shutdown callback
    // hook to InteractiveCliActor (framework change).  Until then, poll at
    // 50 ms — a reasonable tradeoff between responsiveness and CPU waste.
    while (system.cli_actor() && system.cli_actor()->is_running()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // Shutdown the CLI server before system.shutdown() to ensure the
    // daemon thread's event loop is stopped before ActorSystem drains
    // resources the event loop may still reference.
    auto server =
        std::static_pointer_cast<cli::CliProtoServerActor>(cli_server.get());
    if (server) {
        server->request_shutdown();
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
