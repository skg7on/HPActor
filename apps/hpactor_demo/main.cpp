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

/// \file main.cpp
/// \brief hpactor_demo — unified foreground/service binary using cli_demo
/// actors.
///
/// Supports three modes:
///   --foreground  Interactive CLI via stdin + UDS socket (default)
///   --systemd     systemd Type=notify service (Linux only)
///   --daemon      Traditional double-fork daemon (Linux only)

#include "cli_demo_actor_factory.hpp"
#include "daemon_runner.hpp"
#include "foreground_runner.hpp"

#include <hpactor/actor/actor_system.hpp>
#include <hpactor/cli/config/cli_config.hpp>
#include <hpactor/process/process_config.hpp>

#include <cerrno>
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
              << "  --help                Show this help\n";
}

std::string default_uds_path() {
    // /tmp is user-writable on all platforms; /var/run requires root on Linux.
    return "/tmp/hpactor/hpactor.sock";
}

/// Parse a port number from a CLI argument. Prints error and exits on invalid
/// input.
static uint16_t parse_port(const char* arg, const char* flag_name) {
    char* end = nullptr;
    errno = 0;
    long val = std::strtol(arg, &end, 10);
    if (errno != 0 || end == arg || *end != '\0' || val < 0 || val > 65535) {
        std::cerr << "Error: invalid port for " << flag_name << ": " << arg << "\n";
        std::exit(1);
    }
    return static_cast<uint16_t>(val);
}

/// Print an error message and exit when a flag requires an argument but none is
/// provided (flag is the last token on the command line).
[[noreturn]] static void missing_arg(const char* flag_name) {
    std::cerr << "Error: " << flag_name << " requires an argument\n";
    std::exit(1);
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
            if (i + 1 >= argc)
                missing_arg("--config");
            opts.config_path = argv[++i];
        } else if (arg == "--uds-path") {
            if (i + 1 >= argc)
                missing_arg("--uds-path");
            opts.uds_path = argv[++i];
        } else if (arg == "--tcp-port") {
            if (i + 1 >= argc)
                missing_arg("--tcp-port");
            opts.tcp_port = parse_port(argv[++i], "--tcp-port");
        } else if (arg == "--health-port") {
            if (i + 1 >= argc)
                missing_arg("--health-port");
            opts.health_port = parse_port(argv[++i], "--health-port");
        } else if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            std::exit(0);
        } else if (arg == "--log-level") {
            // Accepted for forward-compatibility. Log-level overrides require
            // TOML [system.logging] configuration or environment variables;
            // the CLI flag is not yet wired to LogManager.
            if (i + 1 < argc) {
                opts.log_level = argv[++i];
                std::cerr << "Warning: --log-level is not yet supported; use "
                             "TOML config or environment variables.\n";
            } else {
                missing_arg("--log-level");
            }
        } else {
            std::cerr << "Error: unknown option: " << arg << "\n";
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

    return config;
}

} // namespace

int main(int argc, char* argv[]) {
    auto opts = parse_args(argc, argv);

    // Platform guard: daemon/systemd modes are Linux-only.
    // Must run BEFORE ActorSystem construction because ProcessManager::init()
    // calls daemonize() (double-fork) as a side effect of the constructor.
    if (opts.mode != process::ProcessMode::Foreground) {
#ifdef __APPLE__
        std::cerr << "Error: daemon/systemd mode is not supported on macOS.\n"
                  << "Use --foreground mode instead.\n";
        return 1;
#endif
    }

    // Resolve UDS path once — used by splash, foreground runner, and daemon
    // runner.
    auto uds_path =
        opts.uds_path.empty() ? default_uds_path() : std::move(opts.uds_path);

    // Splash and status messages for foreground mode.
    // MUST be printed BEFORE ActorSystem construction because the ActorSystem
    // constructor spawns the CliActor daemon thread, which immediately puts
    // the terminal in raw mode via linenoise().  Any stdout writes after that
    // point race with the prompt and produce garbled output.
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
            << "║    hpactor-cli --socket " << uds_path << "\n"
            << "║                                                              ║\n"
            << "╚══════════════════════════════════════════════════════════════╝\n"
            << std::endl;

        // Print status lines while stdout is still in cooked mode.
        // These used to appear interleaved with the linenoise prompt
        // because run_foreground() ran after ActorSystem construction.
        std::cout
            << "\n[hpactor_demo foreground mode — type /help for commands, "
               "/quit to exit]\n"
            << "[CliProtoServerActor listening on UDS: " << uds_path << "]\n";
        if (opts.tcp_port > 0)
            std::cout << "[CliProtoServerActor listening on TCP: 0.0.0.0:"
                      << opts.tcp_port << "]\n";
        std::cout << std::endl;
    }

    // Build config and construct ActorSystem
    auto config = build_config(opts);
    ActorSystem system(config);

    // Demo actors are only spawned in foreground (interactive) mode.
    // In daemon/systemd mode they consume scheduler resources with no
    // connected CLI user.  A future enhancement could defer spawning to
    // when the first CLI session attaches.
    if (opts.mode == process::ProcessMode::Foreground) {
        auto actors = cli_demo::spawn_cli_demo_actors(system);
        cli_demo::kickoff_cli_demo_actors(system, actors);
    }

    // Dispatch by mode
    if (opts.mode == process::ProcessMode::Foreground) {
        hpactor_demo::ForegroundConfig fg_cfg;
        fg_cfg.uds_path = uds_path;
        fg_cfg.tcp_port = opts.tcp_port;
        return hpactor_demo::run_foreground(system, fg_cfg);
    }

    // Daemon/systemd mode
    hpactor_demo::DaemonConfig daemon_cfg;
    daemon_cfg.uds_path = uds_path;
    daemon_cfg.tcp_port = opts.tcp_port;
    daemon_cfg.health_port = opts.health_port;
    daemon_cfg.watchdog_interval = (opts.mode == process::ProcessMode::Systemd)
                                       ? std::chrono::milliseconds{5000}
                                       : std::chrono::milliseconds{0};
    return hpactor_demo::run_daemon(system, daemon_cfg);
}
