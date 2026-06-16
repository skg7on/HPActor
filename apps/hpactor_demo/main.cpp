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

#include "cli_demo_actor_factory.hpp"
#include "daemon_runner.hpp"
#include "foreground_runner.hpp"

#include <hpactor/cli/cli_config.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/process/process_config.hpp>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

using namespace hpactor;
using namespace hpactor::apps;

namespace {

struct CliOptions {
    process::ProcessMode mode = process::ProcessMode::Foreground;
    std::string config_path;
    std::string uds_path;
    uint16_t tcp_port = 0;
    uint16_t health_port = 8089;
    std::string log_level;
};

void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog << " [options]\n"
              << "Options:\n"
              << "  --foreground          Interactive CLI mode (default)\n"
              << "  --systemd             systemd Type=notify mode (Linux)\n"
              << "  --daemon              Traditional daemon mode (Linux)\n"
              << "  --config PATH         TOML config file path\n"
              << "  --uds-path PATH       UDS listen path\n"
              << "  --tcp-port PORT       TCP port (0=disabled)\n"
              << "  --health-port PORT    Health HTTP port (default: 8089)\n"
              << "  --log-level LEVEL     Override log level\n"
              << "  --help                Show this help\n";
}

std::string default_uds_path() {
#ifdef __APPLE__
    return "/tmp/hpactor/hpactor.sock";
#else
    return "/var/run/hpactor/hpactor.sock";
#endif
}

CliOptions parse_args(int argc, char* argv[]) {
    CliOptions opts;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--foreground") {
            opts.mode = process::ProcessMode::Foreground;
        } else if (arg == "--systemd") {
            opts.mode = process::ProcessMode::Systemd;
        } else if (arg == "--daemon") {
            opts.mode = process::ProcessMode::Daemon;
        } else if (arg == "--config") {
            if (i + 1 < argc)
                opts.config_path = argv[++i];
        } else if (arg == "--uds-path") {
            if (i + 1 < argc)
                opts.uds_path = argv[++i];
        } else if (arg == "--tcp-port") {
            if (i + 1 < argc)
                opts.tcp_port = static_cast<uint16_t>(std::atoi(argv[++i]));
        } else if (arg == "--health-port") {
            if (i + 1 < argc)
                opts.health_port = static_cast<uint16_t>(std::atoi(argv[++i]));
        } else if (arg == "--log-level") {
            if (i + 1 < argc)
                opts.log_level = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            std::exit(0);
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
            print_usage(argv[0]);
            std::exit(1);
        }
    }
    return opts;
}

Config build_config(const CliOptions& opts) {
    Config config;

    config.scheduler_threads = 4;
    config.max_queue_depth = 1024;

    config.process.mode = opts.mode;

    // Mailbox defaults
    config.mailbox.default_capacity = 256;
    config.mailbox.default_policy = mailbox::OverflowPolicy::DeadLetter;
    config.dead_letters.capacity = 1024;

    // Graceful shutdown
    config.shutdown_drain =
        DrainConfig{DrainPolicy::Drain, std::chrono::milliseconds{30'000}};

    // CLI config for foreground
    if (opts.mode == process::ProcessMode::Foreground) {
        config.cli = cli::CliConfig{.enabled = true,
                                    .listen_path = "",
                                    .tcp_port = 0,
                                    .default_format = "pretty",
                                    .page_size = 20,
                                    .history_path = "",
                                    .history_max = 1000};
    }

    // Note: Config does not have a 'logging' member for log-level overrides.
    // Log level is configured via LogConfig managed internally by ActorSystem.
    // Use TOML config or environment variables for log-level overrides.

    return config;
}

} // namespace

int main(int argc, char* argv[]) {
    auto opts = parse_args(argc, argv);

    // Splash for foreground mode
    if (opts.mode == process::ProcessMode::Foreground) {
        std::cout
            << "\n"
            << "╔══════════════════════════════════════════════════════════════╗\n"
            << "║     HPActor Demo — Unified Foreground/Service Binary         ║\n"
            << "║     CLI Interactive Demo + Daemon Infrastructure             ║\n"
            << "╠══════════════════════════════════════════════════════════════╣\n"
            << "║                                                              ║\n"
            << "║  Architecture:                                               ║\n"
            << "║    10 actors across 8 types                                  ║\n"
            << "║    4 scheduler threads with A2WS work-stealing               ║\n"
            << "║    Dual CLI: stdin (direct) + UDS socket (hpactor-cli)       ║\n"
            << "║                                                              ║\n"
            << "║  Production Features:                                        ║\n"
            << "║    • Rate limiting (Worker-1: 100msg/s, Worker-2: 500msg/s)  ║\n"
            << "║    • Circuit breaker + quarantine (Worker-3, DlqDemoActor)   ║\n"
            << "║    • Delivery failure generation (Worker-4)                  ║\n"
            << "║    • Bounded mailboxes (256 msg, DeadLetter overflow)        ║\n"
            << "║    • Graceful shutdown (30s drain timeout)                   ║\n"
            << "║                                                              ║\n"
            << "║  Connect via hpactor-cli:                                    ║\n"
            << "║    hpactor-cli --socket "
            << (opts.uds_path.empty() ? default_uds_path() : opts.uds_path) << "\n"
            << "║                                                              ║\n"
            << "╚══════════════════════════════════════════════════════════════╝\n"
            << std::endl;
    }

    // Build config and construct ActorSystem
    auto config = build_config(opts);
    ActorSystem system(config);

    // Spawn all cli_demo actors
    auto actors = cli_demo::spawn_cli_demo_actors(system);
    cli_demo::kickoff_cli_demo_actors(system, actors);

    // Dispatch by mode
    if (opts.mode == process::ProcessMode::Foreground) {
        hpactor_demo::ForegroundConfig fg_cfg;
        fg_cfg.uds_path = opts.uds_path.empty() ? default_uds_path() : opts.uds_path;
        return hpactor_demo::run_foreground(system, fg_cfg);
    }

    // Daemon/systemd mode
    hpactor_demo::DaemonConfig daemon_cfg;
    daemon_cfg.uds_path = opts.uds_path.empty() ? default_uds_path() : opts.uds_path;
    daemon_cfg.tcp_port = opts.tcp_port;
    daemon_cfg.health_port = opts.health_port;
    daemon_cfg.watchdog_interval = (opts.mode == process::ProcessMode::Systemd)
                                       ? std::chrono::milliseconds{5000}
                                       : std::chrono::milliseconds{0};
    return hpactor_demo::run_daemon(system, daemon_cfg);
}
