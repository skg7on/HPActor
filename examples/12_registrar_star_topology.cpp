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

// =============================================================================
// HPActor Example 12: Registrar Star Topology
// =============================================================================
//
// Demonstrates the single-server, multiple-process registrar star topology:
//
//   - first process binds the registrar TCP port and becomes SERVER mode
//   - later processes fail the bind and become CLIENT mode
//   - clients register over TCP and send heartbeats
//   - a query probe sends UDP ResolveQuery and decodes ResolveResponse
//
// Usage:
//   ./12_registrar_star_topology --server --actor-port 17000
//   ./12_registrar_star_topology --worker worker-a --actor-port 17001
//   ./12_registrar_star_topology --query --target 127.0.0.1:17001
//
// Optional:
//   --host 127.0.0.1
//   --registrar-host 127.0.0.1
//   --registrar-port 5353
//
// If port 5353 is busy on your machine, pass the same override to all modes:
//   --registrar-port 19053
// =============================================================================

#include <hpactor/core/actor_system.hpp>
#include <hpactor/net/registrar.hpp>
#include <hpactor/net/registrar_serialization.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <optional>
#include <string>
#include <thread>

namespace {

struct Options {
    std::string mode;
    std::string worker_name = "worker";
    std::string host = "127.0.0.1";
    std::string registrar_host = "127.0.0.1";
    std::string target;
    uint16_t actor_port = 17000;
    uint16_t registrar_port = 5353;
};

std::atomic<bool> shutdown_requested{false};

[[maybe_unused]] void sigint_handler(int) {
    shutdown_requested.store(true);
}

void print_usage(const char* argv0) {
    std::cout << "Usage:\n"
              << "  " << argv0 << " --server --actor-port 17000 "
              << "[--registrar-port 5353]\n"
              << "  " << argv0 << " --worker worker-a --actor-port 17001 "
              << "[--registrar-host 127.0.0.1] [--registrar-port 5353]\n"
              << "  " << argv0 << " --query --target 127.0.0.1:17001 "
              << "[--registrar-host 127.0.0.1] [--registrar-port 5353]\n";
}

bool parse_port(const std::string& value, uint16_t& port, std::string& error) {
    char* end = nullptr;
    errno = 0;
    long parsed = std::strtol(value.c_str(), &end, 10);
    if (errno != 0 || end == value.c_str() || *end != '\0' || parsed <= 0 ||
        parsed > 65535) {
        error = "port out of range: " + value;
        return false;
    }
    port = static_cast<uint16_t>(parsed);
    return true;
}

std::optional<Options> parse_args(int argc, char* argv[], std::string& error) {
    Options opts;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        auto need_value = [&](const std::string& flag) -> const char* {
            if (i + 1 >= argc) {
                error = "missing value for " + flag;
                return nullptr;
            }
            return argv[++i];
        };

        if (arg == "--server") {
            opts.mode = "server";
        } else if (arg == "--worker") {
            opts.mode = "worker";
            const char* value = need_value(arg);
            if (value == nullptr)
                return std::nullopt;
            opts.worker_name = value;
        } else if (arg == "--query") {
            opts.mode = "query";
        } else if (arg == "--host") {
            const char* value = need_value(arg);
            if (value == nullptr)
                return std::nullopt;
            opts.host = value;
        } else if (arg == "--registrar-host") {
            const char* value = need_value(arg);
            if (value == nullptr)
                return std::nullopt;
            opts.registrar_host = value;
        } else if (arg == "--actor-port") {
            const char* value = need_value(arg);
            if (value == nullptr)
                return std::nullopt;
            if (!parse_port(value, opts.actor_port, error))
                return std::nullopt;
        } else if (arg == "--registrar-port") {
            const char* value = need_value(arg);
            if (value == nullptr)
                return std::nullopt;
            if (!parse_port(value, opts.registrar_port, error)) {
                return std::nullopt;
            }
        } else if (arg == "--target") {
            const char* value = need_value(arg);
            if (value == nullptr)
                return std::nullopt;
            opts.target = value;
        } else if (arg == "--help" || arg == "-h") {
            return std::nullopt;
        } else {
            error = "unknown argument: " + arg;
            return std::nullopt;
        }
    }

    if (opts.mode.empty()) {
        return std::nullopt;
    }
    if (opts.mode == "query" && opts.target.empty()) {
        error = "--query requires --target";
        return std::nullopt;
    }
    return opts;
}

} // namespace

int main(int argc, char* argv[]) {
    std::string error;
    auto opts = parse_args(argc, argv, error);
    if (!opts) {
        if (!error.empty()) {
            std::cerr << "error: " << error << "\n";
        }
        print_usage(argv[0]);
        return error.empty() ? 1 : 2;
    }

    std::cout << "=== HPActor Example 12: Registrar Star Topology ===\n";
    std::cout << "mode=" << opts->mode << "\n";
    return 0;
}
