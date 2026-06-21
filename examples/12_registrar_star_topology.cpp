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
//   --registrar-port 19053
//
// Default registrar port is 19053 (not 5353). Port 5353 is commonly used by
// mDNS (Bonjour) on macOS and Linux, preventing the UDP resolver socket from
// binding. Override with --registrar-port if 19053 is also in use on your
// machine.
//
// Demo transcript:
//
// Terminal 1:
//   ./12_registrar_star_topology --server --actor-port 17000
//
// Terminal 2:
//   ./12_registrar_star_topology --worker worker-a --actor-port 17001
//
// Terminal 3:
//   ./12_registrar_star_topology --query --target 127.0.0.1:17001
//
// Expected query output:
//   RESOLVED
//     endpoint: 127.0.0.1:17001
// =============================================================================

#include <hpactor/actor/actor_system.hpp>
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
    uint16_t registrar_port = 19053;
};

std::atomic<bool> shutdown_requested{false};

void sigint_handler(int) {
    shutdown_requested.store(true);
}

void print_usage(const char* argv0) {
    std::cout << "Usage:\n"
              << "  " << argv0 << " --server --actor-port 17000 "
              << "[--registrar-port 19053]\n"
              << "  " << argv0 << " --worker worker-a --actor-port 17001 "
              << "[--registrar-host 127.0.0.1] [--registrar-port 19053]\n"
              << "  " << argv0 << " --query --target 127.0.0.1:17001 "
              << "[--registrar-host 127.0.0.1] [--registrar-port 19053]\n";
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

// ── Network config helpers ─────────────────────────────────────────────

hpactor::Config make_network_config(const std::string& host, uint16_t actor_port,
                                    uint16_t registrar_port) {
    hpactor::Config config;
    config.scheduler_threads = 1;
    config.enable_network = true;
    config.endpoint = hpactor::endpoint_ops::parse_endpoint(
        host + ":" + std::to_string(actor_port));
    config.tcp_port = actor_port;
    config.registrar.tcp_port = registrar_port;
    config.registrar.udp_port = registrar_port;
    return config;
}

void add_registrar_static_route(hpactor::Config& config,
                                const std::string& registrar_host,
                                uint16_t registrar_port) {
    config.registrar.static_routes.push_back(hpactor::net::StaticRouteConfig{
        hpactor::endpoint_ops::parse_endpoint(registrar_host + ":" +
                                              std::to_string(registrar_port)),
        registrar_host, registrar_port});
}

// ── Server mode ────────────────────────────────────────────────────────

void run_server(const Options& opts) {
    std::signal(SIGINT, sigint_handler);

    hpactor::Config config =
        make_network_config(opts.host, opts.actor_port, opts.registrar_port);
    hpactor::ActorSystem system(config);

    std::cout << "SERVER mode\n"
              << "  actor endpoint: " << opts.host << ":" << opts.actor_port << "\n"
              << "  actor tcp:      " << opts.actor_port << "\n"
              << "  registrar tcp:  " << opts.registrar_port << "\n"
              << "  registrar udp:  " << opts.registrar_port << "\n"
              << "  press Ctrl-C to stop\n";

    while (!shutdown_requested.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }

    std::cout << "SERVER stopping\n";
}

// ── Worker mode ────────────────────────────────────────────────────────

void run_worker(const Options& opts) {
    std::signal(SIGINT, sigint_handler);

    hpactor::Config config =
        make_network_config(opts.host, opts.actor_port, opts.registrar_port);
    add_registrar_static_route(config, opts.registrar_host, opts.registrar_port);

    hpactor::ActorSystem system(config);

    std::cout << "CLIENT mode\n"
              << "  worker name:    " << opts.worker_name << "\n"
              << "  actor endpoint: " << opts.host << ":" << opts.actor_port << "\n"
              << "  actor tcp:      " << opts.actor_port << "\n"
              << "  registrar host: " << opts.registrar_host << "\n"
              << "  registrar tcp:  " << opts.registrar_port << "\n"
              << "  registrar udp:  " << opts.registrar_port << "\n"
              << "  press Ctrl-C to stop\n";

    while (!shutdown_requested.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }

    std::cout << "CLIENT stopping\n";
}

// ── UDP packet helpers ─────────────────────────────────────────────────

hpactor::StreamBuffer make_udp_packet(hpactor::net::RegistrarMessageType type,
                                      const hpactor::StreamBuffer& payload) {
    hpactor::StreamBuffer packet(hpactor::net::RegistrarHeaderSize + payload.size());

    uint32_t magic_be = htonl(hpactor::net::RegistrarMagic);
    std::memcpy(packet.data(), &magic_be, 4);
    packet[4] = hpactor::net::RegistrarVersion;
    packet[5] = static_cast<uint8_t>(type);
    uint32_t len_be = htonl(static_cast<uint32_t>(payload.size()));
    std::memcpy(packet.data() + 6, &len_be, 4);

    if (!payload.empty()) {
        std::memcpy(packet.data() + hpactor::net::RegistrarHeaderSize,
                    payload.data(), payload.size());
    }
    return packet;
}

bool parse_udp_packet(const hpactor::StreamBuffer& packet,
                      hpactor::net::RegistrarMessageType& type,
                      hpactor::StreamBuffer& payload) {
    if (packet.size() < hpactor::net::RegistrarHeaderSize) {
        return false;
    }

    uint32_t magic = 0;
    std::memcpy(&magic, packet.data(), 4);
    magic = ntohl(magic);
    if (magic != hpactor::net::RegistrarMagic) {
        return false;
    }

    if (packet[4] != hpactor::net::RegistrarVersion) {
        return false;
    }

    uint32_t payload_len = 0;
    std::memcpy(&payload_len, packet.data() + 6, 4);
    payload_len = ntohl(payload_len);
    if (payload_len > packet.size() - hpactor::net::RegistrarHeaderSize) {
        return false;
    }

    type = static_cast<hpactor::net::RegistrarMessageType>(packet[5]);
    payload.assign(packet.begin() + hpactor::net::RegistrarHeaderSize,
                   packet.begin() + hpactor::net::RegistrarHeaderSize + payload_len);
    return true;
}

// ── UDP resolve query ──────────────────────────────────────────────────

std::optional<hpactor::PbResolveResponsePayload>
send_resolve_query(const std::string& registrar_host, uint16_t registrar_port,
                   const std::string& target, std::string& error) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        error = "failed to create UDP socket";
        return std::nullopt;
    }

    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(registrar_port);
    if (inet_pton(AF_INET, registrar_host.c_str(), &dest.sin_addr) != 1) {
        close(sock);
        error = "registrar host must be an IPv4 address";
        return std::nullopt;
    }

    hpactor::EndPoint target_ep = hpactor::endpoint_ops::parse_endpoint(target);
    hpactor::StreamBuffer payload =
        hpactor::net::serialize_resolve_query_payload(target_ep);
    hpactor::StreamBuffer request =
        make_udp_packet(hpactor::net::RegistrarMessageType::ResolveQuery, payload);

    ssize_t sent = sendto(sock, request.data(), request.size(), 0,
                          reinterpret_cast<const sockaddr*>(&dest), sizeof(dest));
    if (sent < 0) {
        close(sock);
        error = "failed to send ResolveQuery";
        return std::nullopt;
    }

    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(sock, &read_fds);

    timeval timeout{};
    timeout.tv_sec = 2;
    timeout.tv_usec = 0;

    int ready = select(sock + 1, &read_fds, nullptr, nullptr, &timeout);
    if (ready <= 0) {
        close(sock);
        return std::nullopt;
    }

    uint8_t buffer[65536];
    ssize_t received = recvfrom(sock, buffer, sizeof(buffer), 0, nullptr, nullptr);
    close(sock);
    if (received <= 0) {
        return std::nullopt;
    }

    hpactor::StreamBuffer response_packet(buffer,
                                          buffer + static_cast<size_t>(received));
    hpactor::net::RegistrarMessageType type;
    hpactor::StreamBuffer response_payload;
    if (!parse_udp_packet(response_packet, type, response_payload)) {
        return std::nullopt;
    }
    if (type != hpactor::net::RegistrarMessageType::ResolveResponse) {
        return std::nullopt;
    }

    hpactor::PbResolveResponsePayload response;
    if (!hpactor::net::parse_resolve_response_payload(response_payload, response)) {
        return std::nullopt;
    }
    return response;
}

// ── Query mode ─────────────────────────────────────────────────────────

void run_query(const Options& opts) {
    std::cout << "QUERY mode\n"
              << "  registrar host: " << opts.registrar_host << "\n"
              << "  registrar udp:  " << opts.registrar_port << "\n"
              << "  target:         " << opts.target << "\n";

    std::string error;
    auto response = send_resolve_query(opts.registrar_host, opts.registrar_port,
                                       opts.target, error);
    if (!error.empty()) {
        std::cerr << "query error: " << error << "\n";
        return;
    }
    if (!response) {
        std::cout << "NOT FOUND\n"
                  << "  endpoint: " << opts.target << "\n";
        return;
    }

    const auto& info = response->endpoint_info();
    std::cout << "RESOLVED\n"
              << "  endpoint: " << info.endpoint() << "\n"
              << "  host:     " << info.host() << "\n"
              << "  tcp_port: " << info.tcp_port() << "\n";
}

} // namespace

// =============================================================================
// Main
// =============================================================================

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

    if (opts->mode == "server") {
        run_server(*opts);
    } else if (opts->mode == "worker") {
        run_worker(*opts);
    } else if (opts->mode == "query") {
        run_query(*opts);
    }
    return 0;
}
