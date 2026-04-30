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

#pragma once

#include <hpactor/net/event_loop.hpp>
#include <hpactor/net/http_types.hpp>
#include <hpactor/ref/actor_address.hpp>
#include <hpactor/ref/actor_ref.hpp>
#include <hpactor/types/types.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace hpactor {

class ActorSystem;

namespace net {

class HttpSerializer;
class HttpParser;

// ---------------------------------------------------------------------------
// RouteRegistry — URL pattern matcher for HTTP routing
// ---------------------------------------------------------------------------
class RouteRegistry {
  public:
    using MessageBuilder =
        std::function<std::pair<ActorAddress, TypedMessage>(const HttpRequest&)>;

    RouteRegistry() = default;

    void add(HttpMethod method, std::string pattern,
             MessageBuilder builder, int priority = 0);

    // Returns the builder for the matching route, or nullptr
    const MessageBuilder* match(HttpMethod method, const std::string& path,
                                HttpRequest& req) const;

    // Returns true if any route is registered
    bool empty() const { return routes_.empty(); }

  private:
    struct Route;
    std::vector<Route> routes_;
};

// ---------------------------------------------------------------------------
// HttpServer — HTTP ingress gateway for actor communication
// ---------------------------------------------------------------------------
class HttpServer {
  public:
    using MessageBuilder = RouteRegistry::MessageBuilder;

    explicit HttpServer(ActorSystem* system);
    ~HttpServer();

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    void listen(uint16_t port, std::string host = "0.0.0.0");
    void stop();

    void route(HttpMethod method, std::string path_pattern,
               MessageBuilder builder, int priority = 0);

    void set_reply_timeout(std::chrono::milliseconds timeout) {
        reply_timeout_ = timeout;
    }
    void set_max_connections(size_t max) { max_connections_ = max; }
    void set_max_request_size(size_t max_bytes) { max_request_size_ = max_bytes; }

    uint16_t port() const { return bound_port_; }
    bool is_running() const { return running_; }

  private:
    struct ConnectionCtx;
    struct PendingReply;

    void on_read(int client_fd);
    void on_parse_complete(int client_fd, HttpRequest&& request);
    void on_reply(TypedMessage&& msg);
    void on_timeout(uint64_t request_id);
    void send_error(int client_fd, HttpStatusCode code,
                    const std::string& message);
    void close_connection(int client_fd);

    ActorSystem* system_;
    EventLoop loop_;
    std::unique_ptr<HttpSerializer> serializer_;
    RouteRegistry routes_;
    std::unordered_map<int, std::unique_ptr<ConnectionCtx>> connections_;
    std::unordered_map<uint64_t, std::unique_ptr<PendingReply>> pending_replies_;
    mutable std::mutex map_mutex_;
    Actor reply_adapter_{nullptr};

    int listening_fd_ = -1;
    uint16_t bound_port_ = 0;
    std::atomic<bool> running_{false};

    std::chrono::milliseconds reply_timeout_{5000};
    size_t max_connections_{1000};
    size_t max_request_size_{1048576};
    uint64_t next_global_request_id_{1};
};

} // namespace net
} // namespace hpactor
