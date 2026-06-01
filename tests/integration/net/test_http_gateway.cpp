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

#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/actor/http_gateway_actor.hpp>
#include <hpactor/actor/typed_message.hpp>
#include <hpactor/behavior.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/net/http_client.hpp>
#include <hpactor/net/http_types.hpp>
#include <hpactor/types/types.hpp>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <string>
#include <thread>

#include <gtest/gtest.h>

using namespace hpactor;
using namespace hpactor::net;

// =============================================================================
// EchoActor — echoes received messages back via context()->reply()
// =============================================================================
class EchoActor final : public EventBasedActor {
  public:
    EchoActor(ActorContext* ctx, ActorSystem& sys) : EventBasedActor(ctx, sys) {
        become(make_behavior());
    }

    Behavior make_behavior() override {
        return Behavior([this](TypedMessage& msg) {
            TypedMessage reply(msg.type_id(), msg.payload());
            context()->reply(std::move(reply));
        });
    }
};

// =============================================================================
// SlowActor — never replies (used for timeout test)
// =============================================================================
class SlowActor final : public EventBasedActor {
  public:
    SlowActor(ActorContext* ctx, ActorSystem& sys) : EventBasedActor(ctx, sys) {
        become(make_behavior());
    }

    Behavior make_behavior() override {
        return Behavior([](TypedMessage& /*msg*/) {
            // Intentionally never calls reply()
        });
    }
};

// =============================================================================
// Helpers
// =============================================================================
namespace {

StreamBuffer make_body(const char* str) {
    StreamBuffer body;
    body.append(reinterpret_cast<const uint8_t*>(str), strlen(str));
    return body;
}

std::string body_to_string(const StreamBuffer& body) {
    return {body.begin(), body.end()};
}

static uint16_t find_available_port() {
    int test_fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = 0;
    socklen_t len = sizeof(addr);
    bind(test_fd, reinterpret_cast<struct sockaddr*>(&addr), len);
    getsockname(test_fd, reinterpret_cast<struct sockaddr*>(&addr), &len);
    uint16_t port = ntohs(addr.sin_port);
    close(test_fd);
    return port;
}

} // namespace

// =============================================================================
// HttpGatewayTest — sets up ActorSystem, HTTPGatewayActor via DaemonActor spawn
// =============================================================================
class HttpGatewayTest : public ::testing::Test {
  protected:
    // Use 2 scheduler threads so the EchoActor and ReplyAdapter can be
    // processed concurrently with the HTTP gateway daemon thread. A longer
    // HTTP reply timeout gives headroom on loaded CI runners.
    Config test_config() {
        Config cfg;
        cfg.scheduler_threads = 2;
        cfg.enable_network = false;
        cfg.cli.enabled = false;
        cfg.tracing.enabled = false;
        cfg.mailbox.default_capacity = 1024;
        return cfg;
    }

    ActorSystem system{test_config()};
    Actor echo_actor;
    Actor slow_actor;
    Actor server_actor;
    net::HTTPGatewayActor* server = nullptr;
    uint16_t port = 0;

    void SetUp() override {
        echo_actor = system.spawn<EchoActor>();
        slow_actor = system.spawn<SlowActor>();
        (void)slow_actor;

        port = find_available_port();

        server_actor = system.spawn<net::HTTPGatewayActor>("0.0.0.0", port);
        server = static_cast<net::HTTPGatewayActor*>(server_actor.get().get());
        server->set_reply_timeout(std::chrono::milliseconds(15000));

        while (!server->is_listening()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        setup_routes();

        // Let the scheduler threads stabilize before accepting requests.
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

  private:
    void setup_routes() {
        server->route(
            HttpMethod::POST, "/echo",
            [this](const HttpRequest& req) -> std::pair<ActorAddress, TypedMessage> {
                TypedMessage msg(TypeTag::User, req.body);
                return {echo_actor.address(), std::move(msg)};
            });

        server->route(
            HttpMethod::POST, "/echo/:name",
            [this](const HttpRequest& req) -> std::pair<ActorAddress, TypedMessage> {
                auto it = req.path_params.find("name");
                std::string name =
                    (it != req.path_params.end()) ? it->second : "unknown";
                StreamBuffer body;
                body.append(reinterpret_cast<const uint8_t*>(name.data()),
                            name.size());
                TypedMessage msg(TypeTag::User, body);
                return {echo_actor.address(), std::move(msg)};
            });

        server->route(
            HttpMethod::POST, "/slow",
            [](const HttpRequest& req) -> std::pair<ActorAddress, TypedMessage> {
                ActorAddress no_one(LocalEndpoint, ActorType{99}, ActorId{99999}, 0);
                TypedMessage msg(TypeTag::User, req.body);
                return {no_one, std::move(msg)};
            });
    }
};

TEST_F(HttpGatewayTest, E2EPostEchoActor) {
    HttpClient client(nullptr);
    std::string url = "http://127.0.0.1:" + std::to_string(port) + "/echo";
    auto future = client.post(url, make_body("hello"));
    auto result = future.get();

    if (!result.has_value()) {
        const auto& err = result.error();
        ADD_FAILURE() << "Error: code=" << err.code() << " msg=" << err.message();
        return;
    }
    ASSERT_TRUE(result.has_value());
    std::string resp = body_to_string(result.value());
    EXPECT_GT(resp.size(), 0u);
    EXPECT_EQ(resp.find("{\"data\":\""), 0u);
}
