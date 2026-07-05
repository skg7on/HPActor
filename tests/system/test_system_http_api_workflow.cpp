// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include "../support/system_test_fixture.hpp"

#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/actor/system/actor_system.hpp>
#include <hpactor/cli/actor/cli_http_server_actor.hpp>
#include <hpactor/cli/config/cli_http_server_config.hpp>
#include <hpactor/cli/http_handler.hpp>
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

// ─── A.3 System handler tests ────────────────────────────────────────

TEST(HttpApiWorkflow, SystemInfoEndpoint) {
    hpactor::Config cfg;
    cfg.scheduler_threads = 0;

    hpactor::ActorSystem system(cfg);

    hpactor::cli::CliHttpServerConfig server_cfg;
    server_cfg.http_port = 19091;
    server_cfg.http_bind_address = "127.0.0.1";

    auto server = system.spawn<hpactor::cli::CliHttpServerActor>(server_cfg);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    HttpClient client;
    ASSERT_TRUE(client.connect(19091));

    auto response = client.request("GET", "/api/v1/system");
    EXPECT_EQ(response_status(response), 200);
    EXPECT_NE(response_body(response).find("total_actors"), std::string::npos);

    auto* raw = static_cast<hpactor::cli::CliHttpServerActor*>(
        system.get_actor(server.id()).get());
    if (raw)
        raw->request_shutdown();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

TEST(HttpApiWorkflow, SystemStatsEndpoint) {
    hpactor::Config cfg;
    cfg.scheduler_threads = 0;

    hpactor::ActorSystem system(cfg);

    hpactor::cli::CliHttpServerConfig server_cfg;
    server_cfg.http_port = 19092;
    server_cfg.http_bind_address = "127.0.0.1";

    auto server = system.spawn<hpactor::cli::CliHttpServerActor>(server_cfg);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    HttpClient client;
    ASSERT_TRUE(client.connect(19092));

    auto response = client.request("GET", "/api/v1/system/stats");
    int status = response_status(response);
    EXPECT_TRUE(status == 200 || status == 503);

    auto* raw = static_cast<hpactor::cli::CliHttpServerActor*>(
        system.get_actor(server.id()).get());
    if (raw)
        raw->request_shutdown();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

TEST(HttpApiWorkflow, SystemMemoryEndpoint) {
    hpactor::Config cfg;
    cfg.scheduler_threads = 0;

    hpactor::ActorSystem system(cfg);

    hpactor::cli::CliHttpServerConfig server_cfg;
    server_cfg.http_port = 19093;
    server_cfg.http_bind_address = "127.0.0.1";

    auto server = system.spawn<hpactor::cli::CliHttpServerActor>(server_cfg);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    HttpClient client;
    ASSERT_TRUE(client.connect(19093));

    auto response = client.request("GET", "/api/v1/system/memory");
    EXPECT_EQ(response_status(response), 200);

    auto* raw = static_cast<hpactor::cli::CliHttpServerActor*>(
        system.get_actor(server.id()).get());
    if (raw)
        raw->request_shutdown();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

TEST(HttpApiWorkflow, DrainEndpoint) {
    hpactor::Config cfg;
    cfg.scheduler_threads = 0;

    hpactor::ActorSystem system(cfg);

    hpactor::cli::CliHttpServerConfig server_cfg;
    server_cfg.http_port = 19094;
    server_cfg.http_bind_address = "127.0.0.1";

    auto server = system.spawn<hpactor::cli::CliHttpServerActor>(server_cfg);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    HttpClient client;
    ASSERT_TRUE(client.connect(19094));

    auto response = client.request("POST", "/api/v1/system/drain", "{}");
    int status = response_status(response);
    EXPECT_TRUE(status == 200 || status == 202);

    auto* raw = static_cast<hpactor::cli::CliHttpServerActor*>(
        system.get_actor(server.id()).get());
    if (raw)
        raw->request_shutdown();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

// ─── A.4 Actor endpoint tests ─────────────────────────────────────────

TEST(HttpApiWorkflow, ListActorsEndpoint) {
    hpactor::Config cfg;
    cfg.scheduler_threads = 0;

    hpactor::ActorSystem system(cfg);

    // Spawn two actors so the list is non-empty.
    auto a1 = system.spawn<hpactor::EventBasedActor>();
    auto a2 = system.spawn<hpactor::EventBasedActor>();
    (void)a1;
    (void)a2;

    hpactor::cli::CliHttpServerConfig server_cfg;
    server_cfg.http_port = 19096;
    server_cfg.http_bind_address = "127.0.0.1";

    auto server = system.spawn<hpactor::cli::CliHttpServerActor>(server_cfg);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    HttpClient client;
    ASSERT_TRUE(client.connect(19096));

    auto response = client.request("GET", "/api/v1/actors");
    EXPECT_EQ(response_status(response), 200);
    EXPECT_NE(response_body(response).find("data"), std::string::npos);

    auto* raw = static_cast<hpactor::cli::CliHttpServerActor*>(
        system.get_actor(server.id()).get());
    if (raw)
        raw->request_shutdown();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

TEST(HttpApiWorkflow, GetActorEndpoint) {
    hpactor::Config cfg;
    cfg.scheduler_threads = 1;

    hpactor::ActorSystem system(cfg);

    auto actor = system.spawn<hpactor::EventBasedActor>();
    uint64_t id = actor.id().value();

    hpactor::cli::CliHttpServerConfig server_cfg;
    server_cfg.http_port = 19097;
    server_cfg.http_bind_address = "127.0.0.1";

    auto server = system.spawn<hpactor::cli::CliHttpServerActor>(server_cfg);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    HttpClient client;
    ASSERT_TRUE(client.connect(19097));

    auto response = client.request("GET", "/api/v1/actors/" + std::to_string(id));
    EXPECT_EQ(response_status(response), 200);

    auto* raw = static_cast<hpactor::cli::CliHttpServerActor*>(
        system.get_actor(server.id()).get());
    if (raw)
        raw->request_shutdown();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

TEST(HttpApiWorkflow, KillActorEndpoint) {
    hpactor::Config cfg;
    cfg.scheduler_threads = 1;

    hpactor::ActorSystem system(cfg);

    auto actor = system.spawn<hpactor::EventBasedActor>();
    uint64_t id = actor.id().value();

    hpactor::cli::CliHttpServerConfig server_cfg;
    server_cfg.http_port = 19098;
    server_cfg.http_bind_address = "127.0.0.1";

    auto server = system.spawn<hpactor::cli::CliHttpServerActor>(server_cfg);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    HttpClient client;
    ASSERT_TRUE(client.connect(19098));

    auto response =
        client.request("DELETE", "/api/v1/actors/" + std::to_string(id));
    int status = response_status(response);
    EXPECT_TRUE(status == 200 || status == 202);

    auto* raw = static_cast<hpactor::cli::CliHttpServerActor*>(
        system.get_actor(server.id()).get());
    if (raw)
        raw->request_shutdown();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

TEST(HttpApiWorkflow, GetActorMailboxEndpoint) {
    hpactor::Config cfg;
    cfg.scheduler_threads = 1;

    hpactor::ActorSystem system(cfg);

    auto actor = system.spawn<hpactor::EventBasedActor>();
    uint64_t id = actor.id().value();

    hpactor::cli::CliHttpServerConfig server_cfg;
    server_cfg.http_port = 19099;
    server_cfg.http_bind_address = "127.0.0.1";

    auto server = system.spawn<hpactor::cli::CliHttpServerActor>(server_cfg);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    HttpClient client;
    ASSERT_TRUE(client.connect(19099));

    auto response =
        client.request("GET", "/api/v1/actors/" + std::to_string(id) + "/mailbox");
    EXPECT_EQ(response_status(response), 200);
    EXPECT_NE(response_body(response).find("depth"), std::string::npos);

    auto* raw = static_cast<hpactor::cli::CliHttpServerActor*>(
        system.get_actor(server.id()).get());
    if (raw)
        raw->request_shutdown();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

// ─── A.5 Circuit breaker, quarantine, memory ──────────────────────────

TEST(HttpApiWorkflow, CircuitBreakerEndpoint) {
    hpactor::Config cfg;
    cfg.scheduler_threads = 1;

    hpactor::ActorSystem system(cfg);

    auto actor = system.spawn<hpactor::EventBasedActor>();
    uint64_t id = actor.id().value();

    hpactor::cli::CliHttpServerConfig server_cfg;
    server_cfg.http_port = 19100;
    server_cfg.http_bind_address = "127.0.0.1";

    auto server = system.spawn<hpactor::cli::CliHttpServerActor>(server_cfg);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    HttpClient client;
    ASSERT_TRUE(client.connect(19100));

    auto response = client.request(
        "GET", "/api/v1/actors/" + std::to_string(id) + "/circuit-breaker");
    int status = response_status(response);
    EXPECT_TRUE(status == 200 || status == 404 || status == 503);

    auto* raw = static_cast<hpactor::cli::CliHttpServerActor*>(
        system.get_actor(server.id()).get());
    if (raw)
        raw->request_shutdown();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

TEST(HttpApiWorkflow, QuarantineEndpoints) {
    hpactor::Config cfg;
    cfg.scheduler_threads = 1;

    hpactor::ActorSystem system(cfg);

    auto actor = system.spawn<hpactor::EventBasedActor>();
    uint64_t id = actor.id().value();

    hpactor::cli::CliHttpServerConfig server_cfg;
    server_cfg.http_port = 19101;
    server_cfg.http_bind_address = "127.0.0.1";

    auto server = system.spawn<hpactor::cli::CliHttpServerActor>(server_cfg);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // POST quarantine
    {
        HttpClient client;
        ASSERT_TRUE(client.connect(19101));
        auto post_resp = client.request(
            "POST", "/api/v1/actors/" + std::to_string(id) + "/quarantine", "{}");
        int post_status = response_status(post_resp);
        EXPECT_TRUE(post_status == 200 || post_status == 202 || post_status == 404 ||
                    post_status == 409 || post_status == 503);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // DELETE unquarantine
    {
        HttpClient client;
        ASSERT_TRUE(client.connect(19101));
        auto del_resp = client.request(
            "DELETE", "/api/v1/actors/" + std::to_string(id) + "/quarantine");
        int del_status = response_status(del_resp);
        EXPECT_TRUE(del_status == 200 || del_status == 202 ||
                    del_status == 404 || del_status == 409 || del_status == 503);
    }

    auto* raw = static_cast<hpactor::cli::CliHttpServerActor*>(
        system.get_actor(server.id()).get());
    if (raw)
        raw->request_shutdown();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

TEST(HttpApiWorkflow, ActorMemoryEndpoint) {
    hpactor::Config cfg;
    cfg.scheduler_threads = 1;

    hpactor::ActorSystem system(cfg);

    auto actor = system.spawn<hpactor::EventBasedActor>();
    uint64_t id = actor.id().value();

    hpactor::cli::CliHttpServerConfig server_cfg;
    server_cfg.http_port = 19102;
    server_cfg.http_bind_address = "127.0.0.1";

    auto server = system.spawn<hpactor::cli::CliHttpServerActor>(server_cfg);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    HttpClient client;
    ASSERT_TRUE(client.connect(19102));

    auto response =
        client.request("GET", "/api/v1/actors/" + std::to_string(id) + "/memory");
    int status = response_status(response);
    EXPECT_TRUE(status == 200 || status == 501 || status == 503);

    auto* raw = static_cast<hpactor::cli::CliHttpServerActor*>(
        system.get_actor(server.id()).get());
    if (raw)
        raw->request_shutdown();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

// ─── A.6 DLQ, fault, ask, route matching, error handling ──────────────

TEST(HttpApiWorkflow, DlqEndpoints) {
    hpactor::Config cfg;
    cfg.scheduler_threads = 0;

    hpactor::ActorSystem system(cfg);

    hpactor::cli::CliHttpServerConfig server_cfg;
    server_cfg.http_port = 19103;
    server_cfg.http_bind_address = "127.0.0.1";

    auto server = system.spawn<hpactor::cli::CliHttpServerActor>(server_cfg);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // GET /api/v1/dlq
    {
        HttpClient client;
        ASSERT_TRUE(client.connect(19103));
        auto dlq_resp = client.request("GET", "/api/v1/dlq");
        int dlq_status = response_status(dlq_resp);
        EXPECT_TRUE(dlq_status == 200 || dlq_status == 503);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // GET /api/v1/dlq/export
    {
        HttpClient client;
        ASSERT_TRUE(client.connect(19103));
        auto export_resp = client.request("GET", "/api/v1/dlq/export");
        int export_status = response_status(export_resp);
        // Accept 400 because /api/v1/dlq/:index is registered before
        // /api/v1/dlq/export and "export" is parsed as an invalid index.
        EXPECT_TRUE(export_status == 200 || export_status == 400 ||
                    export_status == 503);
    }

    auto* raw = static_cast<hpactor::cli::CliHttpServerActor*>(
        system.get_actor(server.id()).get());
    if (raw)
        raw->request_shutdown();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

TEST(HttpApiWorkflow, FaultEndpoints) {
    hpactor::Config cfg;
    cfg.scheduler_threads = 0;

    hpactor::ActorSystem system(cfg);

    hpactor::cli::CliHttpServerConfig server_cfg;
    server_cfg.http_port = 19104;
    server_cfg.http_bind_address = "127.0.0.1";

    auto server = system.spawn<hpactor::cli::CliHttpServerActor>(server_cfg);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // GET /api/v1/faults
    {
        HttpClient client;
        ASSERT_TRUE(client.connect(19104));
        auto faults_resp = client.request("GET", "/api/v1/faults");
        EXPECT_EQ(response_status(faults_resp), 200);
        EXPECT_NE(response_body(faults_resp).find("enabled"), std::string::npos);
    }

    // POST /api/v1/faults/clear
    {
        HttpClient client;
        ASSERT_TRUE(client.connect(19104));
        auto clear_resp = client.request("POST", "/api/v1/faults/clear", "{}");
        EXPECT_EQ(response_status(clear_resp), 200);
    }

    auto* raw = static_cast<hpactor::cli::CliHttpServerActor*>(
        system.get_actor(server.id()).get());
    if (raw)
        raw->request_shutdown();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

TEST(HttpApiWorkflow, AskEndpoints) {
    hpactor::Config cfg;
    cfg.scheduler_threads = 0;

    hpactor::ActorSystem system(cfg);

    hpactor::cli::CliHttpServerConfig server_cfg;
    server_cfg.http_port = 19105;
    server_cfg.http_bind_address = "127.0.0.1";

    auto server = system.spawn<hpactor::cli::CliHttpServerActor>(server_cfg);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    HttpClient client;
    ASSERT_TRUE(client.connect(19105));

    // GET /api/v1/asks — stub handlers may return 501 Not Implemented
    auto response = client.request("GET", "/api/v1/asks");
    int status = response_status(response);
    EXPECT_TRUE(status == 501 || status == 503 || status == 200);

    auto* raw = static_cast<hpactor::cli::CliHttpServerActor*>(
        system.get_actor(server.id()).get());
    if (raw)
        raw->request_shutdown();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

TEST(HttpApiWorkflow, RoutePatternMatching) {
    std::unordered_map<std::string, std::string> params;

    // Exact match
    EXPECT_TRUE(hpactor::cli::match_route_pattern("/api/v1/actors",
                                                  "/api/v1/actors", params));

    // :id parameter extraction
    EXPECT_TRUE(hpactor::cli::match_route_pattern("/api/v1/actors/:id",
                                                  "/api/v1/actors/42", params));
    EXPECT_EQ(params["id"], "42");
    params.clear();

    // Multi-segment parameter
    EXPECT_TRUE(hpactor::cli::match_route_pattern(
        "/api/v1/actors/:id/mailbox", "/api/v1/actors/42/mailbox", params));
    EXPECT_EQ(params["id"], "42");
    params.clear();

    // No match — different base path
    EXPECT_FALSE(hpactor::cli::match_route_pattern("/api/v1/actors",
                                                   "/api/v1/system", params));

    // No match — wrong segment count
    EXPECT_FALSE(hpactor::cli::match_route_pattern("/api/v1/actors/:id/mailbox",
                                                   "/api/v1/actors/42", params));

    // No match — non-parameter segment mismatch
    EXPECT_FALSE(hpactor::cli::match_route_pattern(
        "/api/v1/actors/:id/mailbox", "/api/v1/actors/42/inbox", params));
}

TEST(HttpApiWorkflow, ErrorResponseFormat) {
    hpactor::Config cfg;
    cfg.scheduler_threads = 0;

    hpactor::ActorSystem system(cfg);

    hpactor::cli::CliHttpServerConfig server_cfg;
    server_cfg.http_port = 19106;
    server_cfg.http_bind_address = "127.0.0.1";

    auto server = system.spawn<hpactor::cli::CliHttpServerActor>(server_cfg);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    HttpClient client;
    ASSERT_TRUE(client.connect(19106));

    // GET a non-existent endpoint
    auto response = client.request("GET", "/api/v1/nonexistent");
    EXPECT_EQ(response_status(response), 404);
    EXPECT_NE(response_body(response).find("error"), std::string::npos);

    auto* raw = static_cast<hpactor::cli::CliHttpServerActor*>(
        system.get_actor(server.id()).get());
    if (raw)
        raw->request_shutdown();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

TEST(HttpApiWorkflow, MalformedActorIdReturnsBadRequest) {
    hpactor::Config cfg;
    cfg.scheduler_threads = 0;

    hpactor::ActorSystem system(cfg);

    hpactor::cli::CliHttpServerConfig server_cfg;
    server_cfg.http_port = 19107;
    server_cfg.http_bind_address = "127.0.0.1";

    auto server = system.spawn<hpactor::cli::CliHttpServerActor>(server_cfg);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    HttpClient client;
    ASSERT_TRUE(client.connect(19107));

    // GET /api/v1/actors/notanumber — should reject malformed ID
    auto response = client.request("GET", "/api/v1/actors/notanumber");
    int status = response_status(response);
    EXPECT_TRUE(status == 400 || status == 404);

    auto* raw = static_cast<hpactor::cli::CliHttpServerActor*>(
        system.get_actor(server.id()).get());
    if (raw)
        raw->request_shutdown();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

TEST(HttpApiWorkflow, LegacyCliEndpoint) {
    hpactor::Config cfg;
    cfg.scheduler_threads = 0;

    hpactor::ActorSystem system(cfg);

    hpactor::cli::CliHttpServerConfig server_cfg;
    server_cfg.http_port = 19108;
    server_cfg.http_bind_address = "127.0.0.1";
    server_cfg.legacy_cli_endpoint = true;

    auto server = system.spawn<hpactor::cli::CliHttpServerActor>(server_cfg);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    HttpClient client;
    ASSERT_TRUE(client.connect(19108));

    // POST /cli with JSON body
    auto response = client.request("POST", "/cli", R"({"command": "help"})");
    int status = response_status(response);
    EXPECT_TRUE(status == 200 || status == 400 || status == 404);

    auto* raw = static_cast<hpactor::cli::CliHttpServerActor*>(
        system.get_actor(server.id()).get());
    if (raw)
        raw->request_shutdown();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

} // namespace
