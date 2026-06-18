// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include "../support/system_test_fixture.hpp"

#include <hpactor/cli/cli_http_server_actor.hpp>
#include <hpactor/cli/cli_http_server_config.hpp>
#include <hpactor/cli/http_handler.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/net/http_types.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <string>
#include <thread>

// Forward-declare handler registration functions (linkable symbols in hpactor).
// These are declared in src/cli/cli_http_server_actor.cpp and defined in
// src/cli/handlers/*.cpp.
namespace hpactor::cli::handlers {
void register_fault_handlers();
void register_ask_handlers();
void register_system_handlers();
void register_dlq_handlers();
void register_actor_handlers();
void register_legacy_handler();
} // namespace hpactor::cli::handlers

namespace {

// Simple HTTP client helper for loopback testing
struct HttpClient {
    int sock = -1;

    bool connect(uint16_t port) {
        sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0)
            return false;
        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        if (::connect(sock, reinterpret_cast<struct sockaddr*>(&addr),
                      sizeof(addr)) < 0) {
            close(sock);
            sock = -1;
            return false;
        }
        return true;
    }

    std::string request(const std::string& method, const std::string& path,
                        const std::string& body = "") {
        std::string req = method + " " + path +
                          " HTTP/1.1\r\n"
                          "Host: 127.0.0.1\r\n";
        if (!body.empty()) {
            req += "Content-Type: application/json\r\n";
            req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        }
        req += "Connection: close\r\n\r\n";
        if (!body.empty())
            req += body;
        send(sock, req.c_str(), req.size(), 0);

        char buf[16384];
        ssize_t n = recv(sock, buf, sizeof(buf) - 1, 0);
        if (n <= 0)
            return "";
        buf[n] = '\0';
        return std::string(buf, static_cast<size_t>(n));
    }

    ~HttpClient() {
        if (sock >= 0)
            close(sock);
    }
};

int response_status(const std::string& response) {
    auto sp = response.find(' ');
    if (sp == std::string::npos)
        return -1;
    return std::stoi(response.substr(sp + 1, 3));
}

std::string response_body(const std::string& response) {
    auto pos = response.find("\r\n\r\n");
    if (pos == std::string::npos)
        return "";
    return response.substr(pos + 4);
}

// ─── Tests ────────────────────────────────────────────────────────────

TEST(HttpApiWorkflow, HttpHandlerRegistryPopulated) {
    using namespace hpactor::cli;

    // Populate the handler registry by calling the same registration
    // functions that CliHttpServerActor::init_routes() uses.
    handlers::register_system_handlers();
    handlers::register_fault_handlers();
    handlers::register_ask_handlers();
    handlers::register_dlq_handlers();
    handlers::register_actor_handlers();
    handlers::register_legacy_handler();

    // Check HttpHandlerRegistry
    const auto& routes = HttpHandlerRegistry::instance().routes();
    EXPECT_GE(routes.size(), 1u);
    for (const auto& entry : routes) {
        EXPECT_NE(entry.handler, nullptr);
        EXPECT_FALSE(entry.pattern.empty());
    }
}

TEST(HttpApiWorkflow, HttpServerStartAndApiIndex) {
    hpactor::Config cfg;
    cfg.scheduler_threads = 0;

    hpactor::ActorSystem system(cfg);

    hpactor::cli::CliHttpServerConfig server_cfg;
    server_cfg.http_port = 19090;
    server_cfg.http_bind_address = "127.0.0.1";

    auto server = system.spawn<hpactor::cli::CliHttpServerActor>(server_cfg);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    HttpClient client;
    ASSERT_TRUE(client.connect(19090));

    auto response = client.request("GET", "/api/v1");
    int status = response_status(response);
    EXPECT_EQ(status, 200);

    std::string body = response_body(response);
    EXPECT_NE(body.find("v1"), std::string::npos);

    // Shutdown server
    auto* raw = static_cast<hpactor::cli::CliHttpServerActor*>(
        system.get_actor(server.id()).get());
    if (raw)
        raw->request_shutdown();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

} // namespace
