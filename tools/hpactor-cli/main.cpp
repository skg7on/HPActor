// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

/// \file main.cpp
/// \brief Standalone CLI client for the hpactor daemon.
///
/// Creates a minimal ActorSystem with a CliClientActor that connects
/// to a remote CliProtoServerActor via UDS or TCP using the HPAC Frame
/// CliCommand/CliResponse wire protocol.

#include <hpactor/cli/cli_client_actor.hpp>
#include <hpactor/cli/cli_client_config.hpp>
#include <hpactor/core/actor_system.hpp>

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

static void print_usage(const char* prog) {
    std::cerr
        << "Usage: " << prog << " [options]\n"
        << "Options:\n"
        << "  -s, --socket PATH   Unix domain socket path\n"
        << "                      (default: /var/run/hpactor/hpactor-cli.sock)\n"
        << "  -H, --host HOST     TCP host address (disables UDS)\n"
        << "  -p, --port PORT     TCP port (required with --host)\n"
        << "  --http-port PORT    HTTP JSON transport port (enables HTTP mode)\n"
        << "  -e, --exec CMD      Execute a single command and exit\n"
        << "  -f, --format FMT    Output format (pretty, json, tabular)\n"
        << "  -h, --help          Show this help message\n";
}

int main(int argc, char* argv[]) {
    hpactor::cli::CliClientConfig config;
    std::string exec_cmd;
    bool show_help = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-s" || arg == "--socket") {
            if (i + 1 >= argc) {
                std::cerr << "Error: --socket requires a path\n";
                return 1;
            }
            config.uds_path = argv[++i];
        } else if (arg == "-H" || arg == "--host") {
            if (i + 1 >= argc) {
                std::cerr << "Error: --host requires an address\n";
                return 1;
            }
            config.host = argv[++i];
        } else if (arg == "-p" || arg == "--port") {
            if (i + 1 >= argc) {
                std::cerr << "Error: --port requires a port\n";
                return 1;
            }
            config.port = static_cast<uint16_t>(std::atoi(argv[++i]));
        } else if (arg == "--http-port") {
            if (i + 1 >= argc) {
                std::cerr << "Error: --http-port requires a port\n";
                return 1;
            }
            config.http_port = static_cast<uint16_t>(std::atoi(argv[++i]));
            config.transport = hpactor::cli::CliClientConfig::Transport::HttpJson;
        } else if (arg == "-e" || arg == "--exec") {
            if (i + 1 >= argc) {
                std::cerr << "Error: --exec requires a command\n";
                return 1;
            }
            exec_cmd = argv[++i];
        } else if (arg == "-f" || arg == "--format") {
            if (i + 1 >= argc) {
                std::cerr << "Error: --format requires a format\n";
                return 1;
            }
            config.default_format = argv[++i];
        } else if (arg == "-h" || arg == "--help") {
            show_help = true;
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
            print_usage(argv[0]);
            return 1;
        }
    }

    if (show_help) {
        print_usage(argv[0]);
        return 0;
    }
    if (!config.host.empty() && config.port == 0) {
        std::cerr << "Error: TCP mode requires --port\n";
        return 1;
    }

    hpactor::Config sys_config;
    sys_config.scheduler_threads = 0;
    hpactor::ActorSystem system(sys_config);

    auto client = system.spawn<hpactor::cli::CliClientActor>(config);
    auto* raw = static_cast<hpactor::cli::CliClientActor*>(
        system.get_actor(client.id()).get());

    if (!exec_cmd.empty()) {
        raw->set_exec_command(exec_cmd);
    }

    while (raw->is_running()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    return 0;
}
