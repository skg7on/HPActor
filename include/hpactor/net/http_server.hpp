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

#include <hpactor/actor/daemon_actor.hpp>
#include <hpactor/net/event_loop.hpp>
#include <hpactor/net/http_connection.hpp>
#include <hpactor/net/http_serializer.hpp>
#include <hpactor/net/http_types.hpp>
#include <hpactor/ref/actor_address.hpp>
#include <hpactor/ref/actor_ref.hpp>
#include <hpactor/types/types.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace hpactor {
namespace net {

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
// HTTPServerActor — DaemonActor-based HTTP ingress gateway
// ---------------------------------------------------------------------------
class HTTPServerActor : public DaemonActor {
  public:
    using MessageBuilder = RouteRegistry::MessageBuilder;

    HTTPServerActor(ActorContext* ctx, ActorSystem& sys,
                    const std::string& bind_host, uint16_t port);
    ~HTTPServerActor() override;

    HTTPServerActor(const HTTPServerActor&) = delete;
    HTTPServerActor& operator=(const HTTPServerActor&) = delete;

    // Route registration
    void route(HttpMethod method, std::string path_pattern,
               MessageBuilder builder, int priority = 0);
    void route(HttpMethod method, std::string path_pattern, ActorAddr target);

    // Configuration
    void set_reply_timeout(std::chrono::milliseconds t) { reply_timeout_ = t; }
    void set_max_connections(size_t max) { max_connections_ = max; }
    void set_max_request_size(size_t max) { max_request_size_ = max; }

    uint16_t port() const { return port_; }
    bool is_listening() const { return listen_fd_ >= 0; }

    // DaemonActor overrides
    bool run_once() override;

  protected:
    void on_daemon_start() override;
    void on_daemon_stop() override;
    void on_deactivate() override;

  private:
    void on_accept();
    void on_request(HTTPConnection* conn, HttpRequest&& req);
    void on_reply(TypedMessage&& msg);
    void on_error(HTTPConnection* conn, const error& err);
    void on_timeout(uint64_t request_id);
    void close_connection(HTTPConnection* conn);

    EventLoop loop_;
    RouteRegistry routes_;
    std::unique_ptr<HttpSerializer> serializer_;

    std::unordered_map<HTTPConnection*, HTTPConnectionPtr> connections_;
    std::mutex conn_mutex_;

    struct PendingReply {
        uint64_t request_id;
        HTTPConnection* conn;
        std::chrono::steady_clock::time_point enqueued_at;
    };
    std::unordered_map<uint64_t, std::unique_ptr<PendingReply>> pending_replies_;
    std::mutex reply_mutex_;

    // Thread-safe reply queue — ReplyAdapter enqueues on scheduler thread,
    // drained in run_once() on the daemon thread to avoid races on HTTPConnection.
    std::queue<TypedMessage> reply_queue_;
    std::mutex reply_queue_mutex_;

    Actor reply_adapter_{nullptr};

    std::string bind_host_;
    uint16_t port_;
    int listen_fd_ = -1;
    std::chrono::milliseconds reply_timeout_{5000};
    size_t max_connections_{1000};
    size_t max_request_size_{1048576};
    uint64_t next_request_id_{1};
};

} // namespace net
} // namespace hpactor
