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

#include <hpactor/core/actor_system.hpp>
#include <hpactor/net/http_gateway.hpp>

#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <thread>

namespace hpactor {
namespace net {

// =============================================================================
// RouteRegistry Implementation
// =============================================================================

enum class PatternSegmentType { Literal, NamedParam, SingleWildcard, MultiWildcard };

struct PatternSegment {
    PatternSegmentType type;
    std::string name;
};

struct RouteRegistry::Route {
    HttpMethod method;
    std::vector<PatternSegment> segments;
    MessageBuilder builder;
    int priority;
};

static std::vector<PatternSegment> parse_pattern(const std::string& pattern) {
    std::vector<PatternSegment> segments;
    if (pattern.empty() || pattern[0] != '/') return segments;

    size_t pos = 1;
    while (pos < pattern.size()) {
        size_t end = pattern.find('/', pos);
        std::string seg = pattern.substr(pos, end == std::string::npos ? end : end - pos);
        if (seg.empty()) {
            pos = end == std::string::npos ? pattern.size() : end + 1;
            continue;
        }
        if (seg[0] == '*') {
            if (seg.size() > 1) {
                segments.push_back({PatternSegmentType::MultiWildcard, seg.substr(1)});
            } else {
                segments.push_back({PatternSegmentType::SingleWildcard, ""});
            }
        } else if (seg[0] == ':') {
            segments.push_back({PatternSegmentType::NamedParam, seg.substr(1)});
        } else {
            segments.push_back({PatternSegmentType::Literal, seg});
        }
        pos = end == std::string::npos ? pattern.size() : end + 1;
    }
    return segments;
}

static bool match_pattern(const std::vector<PatternSegment>& segments,
                           const std::string& path, HttpRequest& req) {
    std::vector<std::string> path_segs;
    if (!path.empty() && path[0] == '/') {
        size_t pos = 1;
        while (pos < path.size()) {
            size_t end = path.find('/', pos);
            path_segs.push_back(
                path.substr(pos, end == std::string::npos ? end : end - pos));
            pos = end == std::string::npos ? path.size() : end + 1;
        }
    }

    size_t pi = 0, si = 0;
    while (si < segments.size() && pi < path_segs.size()) {
        const auto& seg = segments[si];
        switch (seg.type) {
        case PatternSegmentType::Literal:
            if (path_segs[pi] != seg.name) return false;
            ++pi; ++si;
            break;
        case PatternSegmentType::NamedParam:
            req.path_params[seg.name] = path_segs[pi];
            ++pi; ++si;
            break;
        case PatternSegmentType::SingleWildcard:
            ++pi; ++si;
            break;
        case PatternSegmentType::MultiWildcard: {
            std::string remaining;
            for (size_t i = pi; i < path_segs.size(); ++i) {
                if (i > pi) remaining += '/';
                remaining += path_segs[i];
            }
            req.path_params[seg.name] = remaining;
            return true;
        }
        }
    }
    return si == segments.size() && pi == path_segs.size();
}

void RouteRegistry::add(HttpMethod method, std::string pattern,
                         MessageBuilder builder, int priority) {
    routes_.push_back(
        {method, parse_pattern(pattern), std::move(builder), priority});
    std::sort(routes_.begin(), routes_.end(),
              [](const Route& a, const Route& b) {
                  return a.priority < b.priority;
              });
}

const RouteRegistry::MessageBuilder*
RouteRegistry::match(HttpMethod method, const std::string& path,
                      HttpRequest& req) const {
    for (const auto& route : routes_) {
        if (route.method != method) continue;
        if (match_pattern(route.segments, path, req)) {
            return &route.builder;
        }
    }
    return nullptr;
}

// =============================================================================
// ReplyAdapter — internal EventBasedActor for receiving actor replies
// =============================================================================
namespace {

class ReplyAdapter final : public EventBasedActor {
  public:
    using ReplyHandler = std::function<void(TypedMessage&&)>;

    ReplyAdapter(ActorContext* ctx, ActorSystem& sys, ReplyHandler handler)
        : EventBasedActor(ctx, sys), handler_(std::move(handler)) {
        become(make_behavior());
    }

    Behavior make_behavior() override {
        return Behavior([this](TypedMessage& msg) {
            if (handler_) {
                handler_(std::move(msg));
            }
        });
    }

  private:
    ReplyHandler handler_;
};

} // anonymous namespace

// =============================================================================
// HTTPGatewayActor Implementation
// =============================================================================

HTTPGatewayActor::HTTPGatewayActor(ActorContext* ctx, ActorSystem& sys,
                                 const std::string& bind_host, uint16_t port)
    : DaemonActor(ctx, sys),
      serializer_(std::make_unique<HttpSerializer>()),
      bind_host_(bind_host),
      port_(port) {

    auto handler = [this](TypedMessage&& msg) {
        std::lock_guard<std::mutex> lock(reply_queue_mutex_);
        reply_queue_.push(std::move(msg));
    };
    reply_adapter_ = system().spawn<ReplyAdapter>(std::move(handler));
}

HTTPGatewayActor::~HTTPGatewayActor() {
    on_deactivate();
}

void HTTPGatewayActor::route(HttpMethod method, std::string path_pattern,
                             MessageBuilder builder, int priority) {
    routes_.add(method, std::move(path_pattern), std::move(builder), priority);
}

void HTTPGatewayActor::route(HttpMethod method, std::string path_pattern,
                             ActorAddr target) {
    auto serializer = serializer_.get();
    route(method, std::move(path_pattern),
          [target, serializer](const HttpRequest& req)
              -> std::pair<ActorAddress, TypedMessage> {
              auto result = serializer->deserialize_request(req, TypeTag::User);
              if (!result.has_value()) {
                  return {invalid_actor_addr, TypedMessage{}};
              }
              return {target, std::move(result.value())};
          });
}

void HTTPGatewayActor::on_daemon_start() {
    // Start the event loop backend BEFORE registering fds via TcpAcceptor.
    // KqueueBackend::start() creates a fresh kqueue fd each call; any fds
    // added before run() would be registered against a kqueue that run()
    // discards.
    loop_.run();

    acceptor_ = std::make_unique<TcpAcceptor>(&loop_);
    acceptor_->set_accept_handler([this](int client_fd, EndPoint remote_endpoint) {
        on_accept(client_fd, remote_endpoint);
    });

    if (!acceptor_->listen(port_, 0, bind_host_)) {
        acceptor_.reset();
        return;
    }
}

void HTTPGatewayActor::on_daemon_stop() {
    {
        std::lock_guard<std::mutex> lock(conn_mutex_);
        for (auto& conn : connections_) {
            conn->close();
        }
        connections_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(reply_mutex_);
        pending_replies_.clear();
    }

    if (acceptor_) {
        acceptor_->close();
    }
    loop_.stop();
}

bool HTTPGatewayActor::run_once() {
    loop_.wait(100);
    loop_.process_completions();

    for (;;) {
        TypedMessage msg;
        {
            std::lock_guard<std::mutex> lock(reply_queue_mutex_);
            if (reply_queue_.empty()) break;
            msg = std::move(reply_queue_.front());
            reply_queue_.pop();
        }
        on_reply(std::move(msg));
    }

    return true;
}

void HTTPGatewayActor::on_deactivate() {
    loop_.stop();
    DaemonActor::on_deactivate();
}

void HTTPGatewayActor::on_accept(int client_fd, EndPoint remote_endpoint) {
    {
        std::lock_guard<std::mutex> lock(conn_mutex_);
        if (connections_.size() >= max_connections_) {
            ::close(client_fd);
            return;
        }
    }

    auto conn = HTTPConnection::create(client_fd, LocalEndpoint,
        remote_endpoint, &loop_, HTTPConnectionMode::Server);

    conn->set_request_handler([this](HTTPConnection* c, HttpRequest&& req) {
        on_request(c, std::move(req));
    });
    conn->set_error_handler([this](HTTPConnection* c, const error& err) {
        on_error(c, err);
    });

    {
        std::lock_guard<std::mutex> lock(conn_mutex_);
        connections_.push_back(std::move(conn));
    }
}

void HTTPGatewayActor::on_request(HTTPConnection* conn, HttpRequest&& req) {
    // Parse query string
    size_t qpos = req.path.find('?');
    if (qpos != std::string::npos) {
        std::string query_str = req.path.substr(qpos + 1);
        req.path = req.path.substr(0, qpos);
        size_t pos = 0;
        while (pos < query_str.size()) {
            size_t eq = query_str.find('=', pos);
            size_t amp = query_str.find('&', pos);
            if (eq != std::string::npos && (amp == std::string::npos || eq < amp)) {
                size_t vstart = eq + 1;
                size_t vend = amp == std::string::npos ? query_str.size() : amp;
                req.query_params[query_str.substr(pos, eq - pos)] =
                    query_str.substr(vstart, vend - vstart);
            }
            pos = amp == std::string::npos ? query_str.size() : amp + 1;
        }
    }

    // Match route
    const auto* builder = routes_.match(req.method, req.path, req);
    if (!builder) {
        StreamBuffer body;
        conn->send_response(HttpStatusCode::NotFound,
            {{"Content-Type", "text/plain"}, {"Connection", "close"}},
            std::move(body));
        close_connection(conn);
        return;
    }

    // Build target + TypedMessage
    auto [target, msg] = (*builder)(req);
    if (!target) {
        StreamBuffer body;
        conn->send_response(HttpStatusCode::InternalError,
            {{"Content-Type", "text/plain"}, {"Connection", "close"}},
            std::move(body));
        close_connection(conn);
        return;
    }

    // Set up reply tracking with correlation ID
    uint64_t request_id = next_request_id_++;
    auto pending = std::make_unique<PendingReply>();
    pending->request_id = request_id;
    pending->conn = conn;
    pending->enqueued_at = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(reply_mutex_);
        pending_replies_[request_id] = std::move(pending);
    }

    // Encode correlation ID into payload prefix (8 bytes BE)
    StreamBuffer correlated;
    for (int i = 7; i >= 0; --i) {
        correlated.push_back(static_cast<uint8_t>((request_id >> (i * 8)) & 0xFF));
    }
    correlated.append(msg.payload().data(), msg.payload().size());

    TypedMessage correlated_msg(msg.type_id(), correlated);
    correlated_msg.set_sender_address(reply_adapter_.address());

    system().deliver_local(target.id, std::move(correlated_msg));

    // Schedule timeout
    int timeout_ms = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(reply_timeout_).count());
    loop_.run_after([this, request_id] { on_timeout(request_id); }, timeout_ms);
}

void HTTPGatewayActor::on_reply(TypedMessage&& msg) {
    const auto& payload = msg.payload();
    if (payload.size() < 8) return;

    uint64_t request_id = 0;
    for (int i = 0; i < 8; ++i) {
        request_id = (request_id << 8) | payload.data()[i];
    }

    HTTPConnection* conn = nullptr;
    {
        std::lock_guard<std::mutex> lock(reply_mutex_);
        auto it = pending_replies_.find(request_id);
        if (it == pending_replies_.end()) return;
        conn = it->second->conn;
        pending_replies_.erase(it);
    }

    if (!conn) return;

    // Strip 8-byte prefix to get actual reply payload
    StreamBuffer reply_payload;
    reply_payload.append(payload.data() + 8, payload.size() - 8);
    TypedMessage reply_msg(msg.type_id(), reply_payload);

    auto [body, content_type] = serializer_->serialize_response(
        reply_msg, "application/json");

    std::vector<HttpHeader> headers = {{"Content-Type", content_type}};

    if (!conn->should_keep_alive()) {
        headers.push_back({"Connection", "close"});
    }

    conn->send_response(HttpStatusCode::OK, std::move(headers), std::move(body));

    if (!conn->should_keep_alive()) {
        close_connection(conn);
    }
}

void HTTPGatewayActor::on_error(HTTPConnection* conn, const error& err) {
    HttpStatusCode code = HttpStatusCode::InternalError;
    if (err.code() == errors::http_parse_error) {
        code = HttpStatusCode::BadRequest;
    }

    StreamBuffer body;
    const auto& msg = err.message();
    if (!msg.empty()) {
        body.append(reinterpret_cast<const uint8_t*>(msg.data()), msg.size());
    }

    conn->send_response(code,
        {{"Content-Type", "text/plain"}, {"Connection", "close"}},
        std::move(body));
    close_connection(conn);
}

void HTTPGatewayActor::on_timeout(uint64_t request_id) {
    HTTPConnection* conn = nullptr;
    {
        std::lock_guard<std::mutex> lock(reply_mutex_);
        auto it = pending_replies_.find(request_id);
        if (it == pending_replies_.end()) return;
        conn = it->second->conn;
        pending_replies_.erase(it);
    }

    if (conn) {
        StreamBuffer body;
        const char* msg_str = "Upstream actor did not respond in time";
        body.append(reinterpret_cast<const uint8_t*>(msg_str), strlen(msg_str));
        conn->send_response(HttpStatusCode::GatewayTimeout,
            {{"Content-Type", "text/plain"}, {"Connection", "close"}},
            std::move(body));
        close_connection(conn);
    }
}

void HTTPGatewayActor::close_connection(HTTPConnection* conn) {
    {
        std::lock_guard<std::mutex> lock(reply_mutex_);
        for (auto it = pending_replies_.begin(); it != pending_replies_.end(); ) {
            if (it->second->conn == conn) {
                it = pending_replies_.erase(it);
            } else {
                ++it;
            }
        }
    }

    std::lock_guard<std::mutex> lock(conn_mutex_);
    auto it = std::find_if(connections_.begin(), connections_.end(),
                           [conn](const HTTPConnectionPtr& p) {
                               return p.get() == conn;
                           });
    if (it != connections_.end()) {
        (*it)->close();
        connections_.erase(it);
    }
}

} // namespace net
} // namespace hpactor
