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
#include <hpactor/actor/typed_message.hpp>
#include <hpactor/behavior.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/net/event_loop.hpp>
#include <hpactor/net/http_parser.hpp>
#include <hpactor/net/http_server.hpp>
#include <hpactor/net/http_serializer.hpp>
#include <hpactor/net/http_types.hpp>
#include <hpactor/types/types.hpp>

#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

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
// ReplyAdapter — internal actor receiving actor replies for HttpServer
// =============================================================================
namespace {

class ReplyAdapter final : public EventBasedActor {
  public:
    using ReplyHandler = std::function<void(TypedMessage&&)>;

    // Constructor matches spawn<T>(args...) expectations:
    //   make_shared<T>(ActorContext*, ActorSystem&, args...)
    ReplyAdapter(ActorContext* ctx, ActorSystem& sys, ReplyHandler handler)
        : EventBasedActor(ctx, sys), handler_(std::move(handler)) {}

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
// HttpServer::ConnectionCtx and PendingReply
// =============================================================================

struct HttpServer::ConnectionCtx {
    int fd = -1;
    std::unique_ptr<HttpParser> parser;
    bytes read_buf;
    std::vector<bytes> write_queue;
    bool keepalive = true;
};

struct HttpServer::PendingReply {
    uint64_t request_id;
    int client_fd;
    std::chrono::steady_clock::time_point enqueued_at;
};

// =============================================================================
// HttpServer Implementation
// =============================================================================

HttpServer::HttpServer(ActorSystem* system) : system_(system) {
    serializer_ = std::make_unique<HttpSerializer>();

    auto handler = [this](TypedMessage&& msg) { on_reply(std::move(msg)); };
    reply_adapter_ = system_->spawn<ReplyAdapter>(std::move(handler));
}

HttpServer::~HttpServer() { stop(); }

void HttpServer::route(HttpMethod method, std::string path_pattern,
                       MessageBuilder builder, int priority) {
    routes_.add(method, std::move(path_pattern), std::move(builder), priority);
}

void HttpServer::listen(uint16_t port, std::string host) {
    listening_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listening_fd_ < 0) return;

    int opt = 1;
    setsockopt(listening_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    fcntl(listening_fd_, F_SETFL, O_NONBLOCK);

    struct sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (host == "0.0.0.0") {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else {
        inet_pton(AF_INET, host.c_str(), &addr.sin_addr);
    }

    if (bind(listening_fd_, reinterpret_cast<struct sockaddr*>(&addr),
             sizeof(addr)) < 0) {
        close(listening_fd_);
        listening_fd_ = -1;
        return;
    }
    if (::listen(listening_fd_, SOMAXCONN) < 0) {
        close(listening_fd_);
        listening_fd_ = -1;
        return;
    }

    bound_port_ = port;
    running_ = true;
    loop_.run();

    loop_.add_fd(listening_fd_, EventLoop::Event::Read);
    loop_.set_read_handler(listening_fd_, [this](int /*fd*/) {
        struct sockaddr_in client_addr {};
        socklen_t client_len = sizeof(client_addr);
        int client_fd = ::accept(
            listening_fd_, reinterpret_cast<struct sockaddr*>(&client_addr),
            &client_len);
        if (client_fd < 0) return;

        fcntl(client_fd, F_SETFL, O_NONBLOCK);

        if (connections_.size() >= max_connections_) {
            close(client_fd);
            return;
        }

        auto ctx = std::make_unique<ConnectionCtx>();
        ctx->fd = client_fd;
        ctx->parser = std::make_unique<HttpParser>();
        ctx->parser->set_on_message(
            [this, fd = client_fd](HttpRequest&& req) {
                on_parse_complete(fd, std::move(req));
            });
        ctx->parser->set_on_error(
            [this, fd = client_fd](llhttp_errno_t /*err*/, const char* msg) {
                send_error(fd, HttpStatusCode::BadRequest,
                           msg ? msg : "Parse error");
            });

        connections_[client_fd] = std::move(ctx);

        loop_.add_fd(client_fd, EventLoop::Event::Read);
        loop_.set_read_handler(client_fd,
                               [this](int cfd) { on_read(cfd); });
    });
}

void HttpServer::stop() {
    if (!running_) return;
    running_ = false;
    loop_.stop();

    for (auto& [fd, ctx] : connections_) {
        close(fd);
    }
    connections_.clear();
    pending_replies_.clear();

    if (listening_fd_ >= 0) {
        close(listening_fd_);
        listening_fd_ = -1;
    }
}

void HttpServer::on_read(int client_fd) {
    auto it = connections_.find(client_fd);
    if (it == connections_.end()) return;

    auto& ctx = it->second;
    uint8_t buf[8192];
    ssize_t n = read(client_fd, buf, sizeof(buf));

    if (n <= 0) {
        close_connection(client_fd);
        return;
    }

    if (ctx->read_buf.size() + static_cast<size_t>(n) > max_request_size_) {
        send_error(client_fd, HttpStatusCode::PayloadTooLarge,
                   "Request entity too large");
        return;
    }

    ctx->read_buf.append(buf, static_cast<size_t>(n));
    ctx->parser->execute(
        std::span<const uint8_t>(ctx->read_buf.data(), ctx->read_buf.size()));

    auto state = ctx->parser->state();
    if (state == HttpParseState::Complete || state == HttpParseState::Idle) {
        ctx->read_buf.clear();
    }
}

void HttpServer::on_parse_complete(int client_fd, HttpRequest&& request) {
    auto it = connections_.find(client_fd);
    if (it == connections_.end()) return;

    auto& ctx = it->second;

    // Parse query string
    size_t qpos = request.path.find('?');
    if (qpos != std::string::npos) {
        std::string query_str = request.path.substr(qpos + 1);
        request.path = request.path.substr(0, qpos);

        size_t pos = 0;
        while (pos < query_str.size()) {
            size_t eq = query_str.find('=', pos);
            size_t amp = query_str.find('&', pos);
            if (eq != std::string::npos && (amp == std::string::npos || eq < amp)) {
                size_t vstart = eq + 1;
                size_t vend = amp == std::string::npos ? query_str.size() : amp;
                request.query_params[query_str.substr(pos, eq - pos)] =
                    query_str.substr(vstart, vend - vstart);
            }
            pos = amp == std::string::npos ? query_str.size() : amp + 1;
        }
    }

    // Match route
    const auto* builder = routes_.match(request.method, request.path, request);
    if (!builder) {
        send_error(client_fd, HttpStatusCode::NotFound,
                   "No route for " + request.path);
        return;
    }

    // Build target + TypedMessage
    auto [target, msg] = (*builder)(request);

    // Set up reply tracking with correlation ID
    uint64_t request_id = next_global_request_id_++;
    auto pending = std::make_unique<PendingReply>();
    pending->request_id = request_id;
    pending->client_fd = client_fd;
    pending->enqueued_at = std::chrono::steady_clock::now();
    pending_replies_[request_id] = std::move(pending);

    // Encode correlation ID into payload prefix (8 bytes BE)
    bytes correlated;
    for (int i = 7; i >= 0; --i) {
        correlated.push_back(
            static_cast<uint8_t>((request_id >> (i * 8)) & 0xFF));
    }
    correlated.append(msg.payload().data(), msg.payload().size());

    TypedMessage correlated_msg(msg.type_id(), correlated);
    // Set reply address to our ReplyAdapter
    correlated_msg.set_sender_address(reply_adapter_.address());

    // Deliver to target actor via ActorSystem
    system_->deliver_local(target.id, std::move(correlated_msg));

    // Track keepalive
    ctx->keepalive = ctx->parser->should_keep_alive();
    ctx->parser->reset();

    // Schedule timeout
    int timeout_ms = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            reply_timeout_).count());
    loop_.run_after([this, request_id] { on_timeout(request_id); }, timeout_ms);
}

void HttpServer::on_reply(TypedMessage&& msg) {
    const auto& payload = msg.payload();
    if (payload.size() < 8) return;

    uint64_t request_id = 0;
    for (int i = 0; i < 8; ++i) {
        request_id = (request_id << 8) | payload.data()[i];
    }

    auto it = pending_replies_.find(request_id);
    if (it == pending_replies_.end()) return;

    int client_fd = it->second->client_fd;
    auto conn_it = connections_.find(client_fd);
    if (conn_it == connections_.end()) {
        pending_replies_.erase(it);
        return;
    }

    // Strip 8-byte prefix to get actual reply payload
    bytes reply_payload;
    reply_payload.append(payload.data() + 8, payload.size() - 8);
    TypedMessage reply_msg(msg.type_id(), reply_payload);

    auto [body, content_type] = serializer_->serialize_response(
        reply_msg, "application/json");

    // Build HTTP response wire format
    std::string status_line = "HTTP/1.1 200 OK\r\n";
    std::string headers;
    headers += "Content-Type: " + content_type + "\r\n";
    headers += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    if (!conn_it->second->keepalive) {
        headers += "Connection: close\r\n";
    }
    headers += "\r\n";

    bytes wire;
    wire.append(reinterpret_cast<const uint8_t*>(status_line.data()),
                status_line.size());
    wire.append(reinterpret_cast<const uint8_t*>(headers.data()),
                headers.size());
    wire.append(body.data(), body.size());

    ssize_t sent = write(client_fd, wire.data(), wire.size());
    (void)sent;

    if (!conn_it->second->keepalive) {
        close_connection(client_fd);
    }

    pending_replies_.erase(it);
}

void HttpServer::on_timeout(uint64_t request_id) {
    auto it = pending_replies_.find(request_id);
    if (it == pending_replies_.end()) return;

    send_error(it->second->client_fd, HttpStatusCode::GatewayTimeout,
               "Upstream actor did not respond in time");
    pending_replies_.erase(it);
}

void HttpServer::send_error(int client_fd, HttpStatusCode code,
                             const std::string& message) {
    std::string status_line = "HTTP/1.1 ";
    status_line += std::to_string(static_cast<uint16_t>(code));
    status_line += " ";
    status_line += reason_phrase(code);
    status_line += "\r\n";

    std::string headers;
    if (code != HttpStatusCode::NoContent) {
        headers += "Content-Type: text/plain\r\n";
        headers += "Content-Length: " + std::to_string(message.size()) + "\r\n";
    }
    headers += "Connection: close\r\n";
    headers += "\r\n";

    bytes wire;
    wire.append(reinterpret_cast<const uint8_t*>(status_line.data()),
                status_line.size());
    wire.append(reinterpret_cast<const uint8_t*>(headers.data()),
                headers.size());
    if (code != HttpStatusCode::NoContent) {
        wire.append(reinterpret_cast<const uint8_t*>(message.data()),
                    message.size());
    }

    ssize_t sent = write(client_fd, wire.data(), wire.size());
    (void)sent;
    close_connection(client_fd);
}

void HttpServer::close_connection(int client_fd) {
    auto it = connections_.find(client_fd);
    if (it == connections_.end()) return;

    loop_.clear_read_handler(client_fd);
    loop_.remove_fd(client_fd);
    close(client_fd);

    for (auto pit = pending_replies_.begin(); pit != pending_replies_.end(); ) {
        if (pit->second->client_fd == client_fd) {
            pit = pending_replies_.erase(pit);
        } else {
            ++pit;
        }
    }

    connections_.erase(it);
}

} // namespace net
} // namespace hpactor
