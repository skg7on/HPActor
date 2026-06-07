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
// HPActor Example 10: Remote PID Query (RPC-Style)
// =============================================================================
//
// Demonstrates cross-process remote spawn and RPC-style queries:
//
//   - spawn_remote() — spawn an actor in another process (blocking)
//   - ActorTypeRegistry::register_type<T>() — register spawnable types
//   - SpawnReceiver — system actor auto-wired for remote spawn
//   - result<ActorRef> error handling pattern
//   - ActorRef as location-transparent handle (wraps ActorProxy for remote)
//   - RPC-style request/response via context()->send() + context()->reply()
//   - Custom serialization for plain C++ structs
//
// Usage:
//   ./10_remote_pid_query --server [port]
//   ./10_remote_pid_query --client <port>
//
// =============================================================================

#include <hpactor/actor/behavior.hpp>
#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/actor_context.hpp>
#include <hpactor/actor_type_registry.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/msg/typed_message.hpp>
#include <hpactor/net/registrar.hpp>
#include <hpactor/spawn.hpp>

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

static const hpactor::TypeTag QueryPidTag{0x00001000};
static const hpactor::TypeTag PidResponseTag{0x00001001};
static const hpactor::TypeTag QueryActorCountTag{0x00001002};
static const hpactor::TypeTag ActorCountResponseTag{0x00001003};
static const hpactor::TypeTag ShutdownMsgTag{0x00001004};
static const hpactor::TypeTag KickTag{0x00001005};

// ---------------------------------------------------------------------------
// Serialization helpers (plain C++ structs, no protobuf)
// ---------------------------------------------------------------------------

static hpactor::StreamBuffer
serialize_pid_response(int pid, const std::string& hostname) {
    hpactor::StreamBuffer b;
    b.push_back(static_cast<uint8_t>((pid >> 24) & 0xFF));
    b.push_back(static_cast<uint8_t>((pid >> 16) & 0xFF));
    b.push_back(static_cast<uint8_t>((pid >> 8) & 0xFF));
    b.push_back(static_cast<uint8_t>(pid & 0xFF));
    b.insert(b.end(), hostname.begin(), hostname.end());
    return b;
}

static std::pair<int, std::string>
deserialize_pid_response(const hpactor::StreamBuffer& b) {
    int pid = (static_cast<int>(b[0]) << 24) | (static_cast<int>(b[1]) << 16) |
              (static_cast<int>(b[2]) << 8) | static_cast<int>(b[3]);
    return {pid, std::string(b.begin() + 4, b.end())};
}

static hpactor::StreamBuffer serialize_actor_count(int count) {
    return {static_cast<uint8_t>((count >> 24) & 0xFF),
            static_cast<uint8_t>((count >> 16) & 0xFF),
            static_cast<uint8_t>((count >> 8) & 0xFF),
            static_cast<uint8_t>(count & 0xFF)};
}

static int deserialize_actor_count(const hpactor::StreamBuffer& b) {
    return (static_cast<int>(b[0]) << 24) | (static_cast<int>(b[1]) << 16) |
           (static_cast<int>(b[2]) << 8) | static_cast<int>(b[3]);
}

// =============================================================================
// ProcessInfoActor — remote-spawnable actor answering system queries
// =============================================================================

class ProcessInfoActor : public hpactor::EventBasedActor {
  public:
    ProcessInfoActor(hpactor::ActorContext* ctx, hpactor::ActorSystem& sys)
        : hpactor::EventBasedActor(ctx, sys) {
        become(make_behavior());
    }

  protected:
    hpactor::Behavior make_behavior() override {
        return hpactor::Behavior{[this](hpactor::TypedMessage& msg) {
            if (msg.type_id() == QueryPidTag) {
                char hostname[256]{};
                gethostname(hostname, sizeof(hostname));
                auto payload =
                    serialize_pid_response(static_cast<int>(getpid()), hostname);
                context()->reply(
                    hpactor::TypedMessage(PidResponseTag, std::move(payload)));
            } else if (msg.type_id() == QueryActorCountTag) {
                int count = static_cast<int>(system().actor_count());
                auto payload = serialize_actor_count(count);
                context()->reply(hpactor::TypedMessage(ActorCountResponseTag,
                                                       std::move(payload)));
            } else if (msg.type_id() == ShutdownMsgTag) {
                set_exit_reason(0);
            }
        }};
    }
};

// =============================================================================
// QueryActor — local actor that sends queries to remote, prints responses
// =============================================================================

class QueryActor : public hpactor::EventBasedActor {
  public:
    QueryActor(hpactor::ActorContext* ctx, hpactor::ActorSystem& sys,
               hpactor::ActorRef remote, std::promise<void> done)
        : hpactor::EventBasedActor(ctx, sys), remote_(std::move(remote)),
          done_(std::move(done)) {
        become(make_behavior());
    }

  protected:
    hpactor::Behavior make_behavior() override {
        return hpactor::Behavior{[this](hpactor::TypedMessage& msg) {
            switch (step_) {
                case Step::WaitKick:
                    if (msg.type_id() == KickTag) {
                        step_ = Step::WaitPid;
                        context()->send(remote_.address(),
                                        hpactor::TypedMessage(
                                            QueryPidTag, hpactor::StreamBuffer{}));
                    }
                    break;
                case Step::WaitPid:
                    if (msg.type_id() == PidResponseTag) {
                        auto [pid, hostname] =
                            deserialize_pid_response(msg.payload());
                        std::cout << "  Remote PID: " << pid << std::endl;
                        std::cout << "  Remote hostname: " << hostname << std::endl;
                        step_ = Step::WaitActorCount;
                        context()->send(
                            remote_.address(),
                            hpactor::TypedMessage(QueryActorCountTag,
                                                  hpactor::StreamBuffer{}));
                    }
                    break;
                case Step::WaitActorCount:
                    if (msg.type_id() == ActorCountResponseTag) {
                        int count = deserialize_actor_count(msg.payload());
                        std::cout << "  Remote actor count: " << count << std::endl;
                        step_ = Step::Done;
                        context()->send(
                            remote_.address(),
                            hpactor::TypedMessage(ShutdownMsgTag,
                                                  hpactor::StreamBuffer{}));
                        done_.set_value();
                    }
                    break;
                default:
                    break;
            }
        }};
    }

  private:
    enum class Step { WaitKick, WaitPid, WaitActorCount, Done };
    hpactor::ActorRef remote_;
    Step step_ = Step::WaitKick;
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

    system.actor_type_registry().register_type<ProcessInfoActor>("process_"
                                                                 "info");

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

    // Establish TCP connection before remote spawn
    auto server_ep = hpactor::endpoint_ops::parse_endpoint(endpoint_str);
    auto conn = system.transport()->connect(server_ep, "127.0.0.1", port);
    if (!conn) {
        std::cerr << "CLIENT: failed to connect to server at " << endpoint_str
                  << std::endl;
        return;
    }
    std::cout << "CLIENT: connected to server" << std::endl;

    // Spawn ProcessInfoActor on the server
    std::cout << "CLIENT: spawning remote ProcessInfoActor..." << std::endl;
    auto result = system.spawn_remote(endpoint_str, "process_info",
                                      hpactor::StreamBuffer{});
    if (!result.has_value()) {
        std::cerr << "CLIENT: spawn_remote failed: " << result.error().message()
                  << std::endl;
        return;
    }
    hpactor::ActorRef remote_ref = result.value();
    std::cout << "CLIENT: remote actor spawned (id="
              << remote_ref.address().id.value() << ")" << std::endl;

    // Spawn local QueryActor to send queries
    std::promise<void> done;
    auto done_future = done.get_future();
    auto query = system.spawn<QueryActor>(std::move(remote_ref), std::move(done));

    // Kick the query actor
    system.deliver_local(query.id(),
                         hpactor::TypedMessage(KickTag, hpactor::StreamBuffer{}));

    auto status = done_future.wait_for(std::chrono::seconds(10));
    if (status == std::future_status::timeout) {
        std::cerr << "CLIENT: timed out waiting for query responses" << std::endl;
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
            port = 17010;
        std::cout << "=== HPActor Example 10: Remote PID Query ===" << std::endl;
        run_server(port);
    } else if (mode == "--client") {
        if (port == 0)
            port = 17010;
        std::cout << "=== HPActor Example 10: Remote PID Query ===" << std::endl;
        run_client(port);
    } else {
        std::cout << "Usage: " << argv[0]
                  << " --server [port] | --client <port>" << std::endl;
        return 1;
    }

    return 0;
}
