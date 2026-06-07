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
// HPActor Example 09: Cross-Process Echo
// =============================================================================
//
// Demonstrates cross-process actor communication using TCP transport:
//
//   - Config::enable_network, tcp_port, endpoint for network setup
//   - RegistrarConfig::static_routes for node discovery
//   - context()->send() to a remote ActorAddress (automatic ActorProxy)
//   - context()->reply() transparently across process boundaries
//   - Local ClientActor as both sender and reply receiver
//
// Usage:
//   ./09_cross_process_echo --server [port]
//   ./09_cross_process_echo --client <port>
//
// =============================================================================

#include <hpactor/actor/behavior.hpp>
#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/actor_context.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/msg/typed_message.hpp>
#include <hpactor/net/registrar.hpp>

#include <atomic>
#include <csignal>
#include <cstring>
#include <future>
#include <iostream>
#include <string>
#include <thread>
#include <unistd.h>

// ---------------------------------------------------------------------------
// Message type tags
// ---------------------------------------------------------------------------

static const hpactor::TypeTag EchoMsgTag{0x00001000};
static const hpactor::TypeTag KickTag{0x00001001};

// ---------------------------------------------------------------------------
// String message helpers
// ---------------------------------------------------------------------------

static hpactor::TypedMessage
make_string_msg(hpactor::TypeTag tag, const std::string& text) {
    hpactor::StreamBuffer payload(text.begin(), text.end());
    return hpactor::TypedMessage(tag, std::move(payload));
}

static std::string extract_string(const hpactor::StreamBuffer& payload) {
    return {payload.begin(), payload.end()};
}

// =============================================================================
// EchoActor — replies "echo: <text>" to every EchoMsgTag
// =============================================================================

class EchoActor : public hpactor::EventBasedActor {
  public:
    EchoActor(hpactor::ActorContext* ctx, hpactor::ActorSystem& sys)
        : hpactor::EventBasedActor(ctx, sys) {
        become(make_behavior());
    }

  protected:
    hpactor::Behavior make_behavior() override {
        return hpactor::Behavior{[this](hpactor::TypedMessage& msg) {
            if (msg.type_id() == EchoMsgTag) {
                auto text = extract_string(msg.payload());
                std::cout << "  EchoActor [" << id().value() << "]: received \""
                          << text << "\"" << std::endl;
                context()->reply(make_string_msg(EchoMsgTag, "echo: " + text));
            }
        }};
    }
};

// =============================================================================
// ClientActor — sends messages to a remote target on kick, prints replies
// =============================================================================

class ClientActor : public hpactor::EventBasedActor {
  public:
    ClientActor(hpactor::ActorContext* ctx, hpactor::ActorSystem& sys,
                hpactor::ActorAddress target, int expected, std::promise<void> done)
        : hpactor::EventBasedActor(ctx, sys), target_(target),
          expected_(expected), done_(std::move(done)) {
        become(make_behavior());
    }

  protected:
    hpactor::Behavior make_behavior() override {
        return hpactor::Behavior{[this](hpactor::TypedMessage& msg) {
            if (msg.type_id() == KickTag) {
                // Kick received — start sending messages to remote
                context()->send(target_, make_string_msg(EchoMsgTag, "hello"));
                context()->send(target_, make_string_msg(EchoMsgTag, "world"));
                context()->send(target_, make_string_msg(EchoMsgTag, "cross-"
                                                                     "proces"
                                                                     "s"));
            } else if (msg.type_id() == EchoMsgTag) {
                std::cout << "  client received: \""
                          << extract_string(msg.payload()) << "\"" << std::endl;
                if (++received_ >= expected_) {
                    done_.set_value();
                }
            }
        }};
    }

  private:
    hpactor::ActorAddress target_;
    int expected_;
    int received_ = 0;
    std::promise<void> done_;
};

// ---------------------------------------------------------------------------
// Server
// ---------------------------------------------------------------------------

static std::atomic<bool> shutdown_requested{false};

static void sigint_handler(int) {
    shutdown_requested.store(true);
}

static void run_server(uint16_t port) {
    std::signal(SIGINT, sigint_handler);

    std::string endpoint_str = "127.0.0.1:" + std::to_string(port);

    hpactor::Config config;
    config.scheduler_threads = 1;
    config.enable_network = true;
    config.endpoint = hpactor::endpoint_ops::parse_endpoint(endpoint_str);
    config.tcp_port = port;

    hpactor::ActorSystem system(config);

    auto echo = system.spawn<EchoActor>();
    std::cout << "SERVER: pid=" << getpid() << " endpoint=" << endpoint_str
              << std::endl;

    while (!shutdown_requested.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::cout << "SERVER: shutting down" << std::endl;
}

// ---------------------------------------------------------------------------
// Client
// ---------------------------------------------------------------------------

static void run_client(uint16_t port) {
    std::string endpoint_str = "127.0.0.1:" + std::to_string(port);

    hpactor::Config config;
    config.scheduler_threads = 1;
    config.enable_network = true;
    config.endpoint = hpactor::endpoint_ops::parse_endpoint("127.0.0.1:0");
    config.tcp_port = 0;
    config.registrar.static_routes.push_back(hpactor::net::StaticRouteConfig{
        hpactor::Ipv4Endpoint{}, "127.0.0.1", port});

    hpactor::ActorSystem system(config);

    auto server_ep = hpactor::endpoint_ops::parse_endpoint(endpoint_str);
    hpactor::ActorAddress echo_addr{server_ep, hpactor::ActorType{0},
                                    hpactor::ActorId{1}, 0};

    // Establish TCP connection to the server before sending messages.
    auto conn = system.transport()->connect(server_ep, "127.0.0.1", port);
    if (!conn) {
        std::cerr << "CLIENT: failed to connect to server at " << endpoint_str
                  << std::endl;
        return;
    }
    std::cout << "CLIENT: connected to server" << std::endl;

    std::promise<void> done;
    auto done_future = done.get_future();
    auto client = system.spawn<ClientActor>(echo_addr, 3, std::move(done));

    // Kick the client actor to start sending
    system.deliver_local(client.id(),
                         hpactor::TypedMessage(KickTag, hpactor::StreamBuffer{}));

    // Wait for client to receive all replies or timeout
    auto status = done_future.wait_for(std::chrono::seconds(5));
    if (status == std::future_status::timeout) {
        std::cerr << "CLIENT: timed out waiting for replies" << std::endl;
    }
}

// =============================================================================
// Main
// =============================================================================

int main(int argc, char* argv[]) {
    std::string mode = argc > 1 ? argv[1] : "--help";
    uint16_t port = 0;
    if (argc > 2) {
        port = static_cast<uint16_t>(std::stoi(argv[2]));
    }

    if (mode == "--server") {
        if (port == 0)
            port = 17009;
        std::cout << "=== HPActor Example 09: Cross-Process Echo ===" << std::endl;
        run_server(port);
    } else if (mode == "--client") {
        if (port == 0)
            port = 17009;
        std::cout << "=== HPActor Example 09: Cross-Process Echo ===" << std::endl;
        run_client(port);
    } else {
        std::cout << "Usage: " << argv[0]
                  << " --server [port] | --client <port>" << std::endl;
        return 1;
    }

    return 0;
}
