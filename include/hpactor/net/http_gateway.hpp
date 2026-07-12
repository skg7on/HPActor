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

/// \brief Segment type for URL pattern matching.
enum class PatternSegmentType {
    Literal,        ///< Exact string match.
    NamedParam,     ///< Named parameter (e.g., \c {id}).
    SingleWildcard, ///< Single path segment wildcard (e.g., \c *).
    MultiWildcard,  ///< Multi-segment wildcard (e.g., \c **).
};

/// \brief A single segment in a route pattern.
struct PatternSegment {
    /// \brief Type of matching for this segment.
    PatternSegmentType type;
    /// \brief Parameter name for \c NamedParam segments.
    std::string name;
};

/// \brief URL pattern-based HTTP route registry.
///
/// Matches incoming HTTP requests against registered URL patterns and
/// returns a \c MessageBuilder that converts the request into an actor
/// message. Routes are evaluated in priority order.
///
/// \note Thread safety: Not internally synchronized. Callers must serialize
///       \c add() and \c match() calls, or call \c add() only during setup
///       before concurrent reads begin.
class RouteRegistry {
  public:
    /// \brief Converts an HTTP request into a target actor address and
    /// typed message.
    ///
    /// \param[in] req Parsed HTTP request with path params populated.
    /// \return Pair of (target actor address, typed message to deliver).
    using MessageBuilder =
        std::function<std::pair<ActorAddress, TypedMessage>(const HttpRequest&)>;

    RouteRegistry() = default;

    /// \brief Register a route.
    ///
    /// \param[in] method HTTP method this route responds to.
    /// \param[in] pattern URL pattern with named params (e.g.,
    ///            \c "/api/users/{id}").
    /// \param[in] builder Callback that constructs the actor message.
    /// \param[in] priority Higher-priority routes are tried first
    ///            (default 0).
    void add(HttpMethod method, std::string pattern, MessageBuilder builder,
             int priority = 0);

    /// \brief Match a request against registered routes.
    ///
    /// Populates \c req.path_params with extracted values on match.
    /// \param[in] method HTTP method.
    /// \param[in] path Request path.
    /// \param[in,out] req Request whose \c path_params will be populated.
    /// \return Pointer to the matching \c MessageBuilder, or \c nullptr
    ///         if no route matches.
    const MessageBuilder*
    match(HttpMethod method, const std::string& path, HttpRequest& req) const;

    /// \brief Check whether any routes are registered.
    ///
    /// \return \c true if the registry is empty.
    bool empty() const {
        return routes_.empty();
    }

  private:
    /// \brief A single registered route entry.
    struct Route {
        HttpMethod method;
        std::vector<PatternSegment> segments;
        MessageBuilder builder;
        int priority;
    };
    std::vector<Route> routes_;
};

/// \brief Reusable HTTP/1.1 server — I/O and protocol only, no actor
/// coupling.
///
/// Uses a shared \c EventLoop (owned by \c NetworkRuntime) for async I/O:
/// \c TcpAcceptor, and a set of \c HTTPConnection instances. The event
/// loop is driven externally; this class no longer calls \c run() or
/// \c stop() on it.
///
/// \note Thread safety: \c listen(), \c stop(), and
///       \c send_response() must all be called from the event loop thread.
class HTTPGateway {
  public:
    /// \brief Callback for incoming HTTP requests.
    ///
    /// \param[in] conn Connection the request arrived on (for sending
    ///            the response).
    /// \param[in] req Parsed HTTP request.
    using RequestHandler = std::function<void(HTTPConnection*, HttpRequest&&)>;

    /// \brief Callback for connection errors.
    ///
    /// \param[in] conn Connection that encountered the error.
    /// \param[in] err Error details.
    using ErrorHandler = std::function<void(HTTPConnection*, const error&)>;

    /// \brief Default constructor — creates an internal EventLoop.
    ///
    /// Use for standalone usage (tests, CLI servers without networking).
    /// For production, prefer \c HTTPGateway(EventLoop*) to share the
    /// \c NetworkRuntime event loop.
    HTTPGateway();

    /// \brief Construct with a shared event loop.
    ///
    /// \param[in] loop Event loop for async I/O (not owned, must outlive
    ///            this gateway). If \c nullptr, an internal loop is created.
    explicit HTTPGateway(EventLoop* loop);
    ~HTTPGateway();

    /// \name Non-copyable
    /// @{
    HTTPGateway(const HTTPGateway&) = delete;
    HTTPGateway& operator=(const HTTPGateway&) = delete;
    /// @}

    /// \brief Start listening for HTTP connections.
    ///
    /// The shared event loop must already be running (managed by
    /// \c NetworkRuntime). Registers the acceptor fd on the shared loop.
    /// \param[in] port TCP port to bind.
    /// \param[in] bind_host IPv4 address to bind (default \c "0.0.0.0").
    /// \return \c true on success.
    bool listen(uint16_t port, const std::string& bind_host = "0.0.0.0");

    /// \brief Stop listening and close all connections.
    ///
    /// Does not stop the event loop — that is managed by
    /// \c NetworkRuntime.
    void stop();

    /// \brief Check whether the server is listening.
    ///
    /// \return \c true if the acceptor is active.
    bool is_listening() const;

    /// \brief Return the bound port.
    ///
    /// \return TCP port number, or 0 if not listening.
    uint16_t port() const;

    /// \brief Process one iteration of the event loop.
    ///
    /// Blocks until an event arrives or the timeout expires on the
    /// shared event loop. Call repeatedly from a daemon thread that
    /// participates in driving the shared loop.
    /// \note Thread safety: Event loop thread only.
    /// \deprecated Prefer letting \c NetworkRuntime drive the shared
    ///             loop. Use only when a daemon thread must participate.
    void run_once();

    /// \brief Set the incoming request handler.
    ///
    /// \param[in] handler Callback invoked for each parsed HTTP request.
    void set_request_handler(RequestHandler handler);

    /// \brief Set the error handler.
    ///
    /// \param[in] handler Callback invoked on connection errors.
    void set_error_handler(ErrorHandler handler);

    /// \brief Send an HTTP response on a connection.
    ///
    /// \param[in] conn Connection to send on.
    /// \param[in] code HTTP status code.
    /// \param[in] headers Response headers.
    /// \param[in] body Response body.
    void send_response(HTTPConnection* conn, HttpStatusCode code,
                       std::vector<HttpHeader> headers, StreamBuffer body);

    /// \brief Close a connection and remove it from tracking.
    ///
    /// \param[in] conn Connection to close.
    void close_connection(HTTPConnection* conn);

    /// \brief Set the maximum number of concurrent connections.
    ///
    /// \param[in] max Maximum connection count (default 1000).
    void set_max_connections(size_t max);

    /// \brief Set the maximum HTTP request body size in bytes.
    ///
    /// \param[in] max Maximum request size (default 1 MiB).
    void set_max_request_size(size_t max);

    /// \brief Access the shared event loop.
    ///
    /// \return Pointer to the shared \c EventLoop (owned by
    ///         \c NetworkRuntime).
    /// \note Thread safety: The returned pointer is safe only for the
    ///       event loop thread.
    EventLoop* event_loop() {
        return loop_;
    }

  private:
    void on_accept(int client_fd, EndPoint remote_endpoint);

    EventLoop* loop_ = nullptr; ///< Active event loop (shared or internal).
    std::unique_ptr<EventLoop> own_loop_; ///< Internal loop when no shared loop
                                          ///< provided.
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
