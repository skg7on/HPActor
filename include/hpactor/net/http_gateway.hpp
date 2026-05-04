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

#include <hpactor/net/acceptor.hpp>
#include <hpactor/net/event_loop.hpp>
#include <hpactor/net/http_connection.hpp>
#include <hpactor/net/http_types.hpp>
#include <hpactor/ref/actor_address.hpp>
#include <hpactor/types/types.hpp>

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace hpactor {
namespace net {

// ---------------------------------------------------------------------------
// RouteRegistry — URL pattern matcher for HTTP routing
// ---------------------------------------------------------------------------

enum class PatternSegmentType { Literal, NamedParam, SingleWildcard, MultiWildcard };

struct PatternSegment {
    PatternSegmentType type;
    std::string name;
};

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
    struct Route {
        HttpMethod method;
        std::vector<PatternSegment> segments;
        MessageBuilder builder;
        int priority;
    };
    std::vector<Route> routes_;
};

// ---------------------------------------------------------------------------
// HTTPGateway — reusable HTTP server (I/O and protocol, no actor coupling)
// ---------------------------------------------------------------------------
class HTTPGateway {
  public:
    using RequestHandler =
        std::function<void(HTTPConnection*, HttpRequest&&)>;
    using ErrorHandler =
        std::function<void(HTTPConnection*, const error&)>;

    HTTPGateway();
    ~HTTPGateway();

    HTTPGateway(const HTTPGateway&) = delete;
    HTTPGateway& operator=(const HTTPGateway&) = delete;

    // Lifecycle — call listen() before run_once()
    bool listen(uint16_t port, const std::string& bind_host = "0.0.0.0");
    void stop();
    bool is_listening() const;
    uint16_t port() const;

    // Main loop tick — call from daemon thread
    void run_once();

    // Handlers
    void set_request_handler(RequestHandler handler);
    void set_error_handler(ErrorHandler handler);

    // Send HTTP response on a connection
    void send_response(HTTPConnection* conn, HttpStatusCode code,
                       std::vector<HttpHeader> headers, StreamBuffer body);

    // Connection management
    void close_connection(HTTPConnection* conn);
    void set_max_connections(size_t max);
    void set_max_request_size(size_t max);

    // Access to event loop for scheduling timers (used by actor for timeouts)
    EventLoop& event_loop() { return loop_; }

  private:
    void on_accept(int client_fd, EndPoint remote_endpoint);

    EventLoop loop_;
    std::unique_ptr<TcpAcceptor> acceptor_;
    std::vector<HTTPConnectionPtr> connections_;
    std::mutex conn_mutex_;
    RequestHandler request_handler_;
    ErrorHandler error_handler_;
    std::string bind_host_;
    uint16_t port_{0};
    size_t max_connections_{1000};
    size_t max_request_size_{1048576};
};

} // namespace net
} // namespace hpactor
