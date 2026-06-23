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

#include "daemon_runner.hpp"

#include <hpactor/actor/actor_system.hpp>
#include <hpactor/cli/cli_proto_server_actor.hpp>
#include <hpactor/cli/cli_proto_server_config.hpp>
#include <hpactor/process/health_check.hpp>
#include <hpactor/process/health_http_server.hpp>
#include <hpactor/process/process_manager.hpp>
#include <hpactor/process/watchdog_actor.hpp>

#include <chrono>
#include <iostream>
#include <thread>

namespace hpactor::apps::hpactor_demo {

int run_daemon(ActorSystem& system, const DaemonConfig& cfg) {
#ifdef __APPLE__
    (void)system;
    (void)cfg;
    std::cerr << "Error: daemon/systemd mode is not supported on macOS.\n"
              << "Use --foreground mode instead.\n";
    return 1;
#else
    // Spawn CliProtoServerActor (sole CLI access path).
    // CliProtoServerActor speaks the HPAC Frame protobuf wire protocol used by
    // CliClientActor (hpactor-cli tool).
    cli::CliProtoServerConfig server_cfg;
    server_cfg.uds_listen_path = cfg.uds_path;
    server_cfg.tcp_listen_port = cfg.tcp_port;
    server_cfg.max_sessions = 16;
    server_cfg.default_format = "pretty";
    server_cfg.page_size = 50;
    system.spawn<cli::CliProtoServerActor>(server_cfg);

    // Spawn WatchdogActor if configured.
    // Store the handle so we can share its health state with the
    // HealthHttpServer below.
    Actor watchdog_actor;
    if (cfg.watchdog_interval.count() > 0) {
        watchdog_actor = system.spawn<process::WatchdogActor>(
            cfg.watchdog_interval, cfg.health_check);
    }

    // Spawn HealthHttpServer if configured
    if (cfg.health_port > 0) {
        process::HealthHttpConfig health_cfg;
        health_cfg.port = cfg.health_port;
        auto health_server = system.spawn<process::HealthHttpServer>(health_cfg);

        // Wire the watchdog's health state into the HTTP server so
        // endpoints reflect real system health.
        if (watchdog_actor) {
            auto* raw_wd = static_cast<process::WatchdogActor*>(
                system.get_actor(watchdog_actor.id()).get());
            auto* raw_hs = static_cast<process::HealthHttpServer*>(
                system.get_actor(health_server.id()).get());
            if (raw_wd && raw_hs) {
                raw_hs->set_health_state(raw_wd->health_state());
            }
        }
    }

    // Yield briefly so DaemonActor threads start and bind sockets before
    // we signal readiness to systemd.  Without this, systemd may mark the
    // unit active and launch dependent services before the UDS socket or
    // TCP port is accepting connections.
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // Signal readiness to systemd
    process::ProcessManager::notify_ready();

    // Block until a signal arrives
    process::ProcessManager::wait_for_signal();

    // Begin graceful shutdown
    process::ProcessManager::notify_stopping();

    auto shutdown_result = system.shutdown();
    if (shutdown_result.has_value()) {
        process::ProcessManager::notify_status("Shutdown complete");
    }

    process::ProcessManager::notify_stopped();
    return 0;
#endif
}

} // namespace hpactor::apps::hpactor_demo
