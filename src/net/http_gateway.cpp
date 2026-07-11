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

#include <hpactor/actor/system/actor_system.hpp>
#include <hpactor/net/http_gateway.hpp>

#include <unistd.h>

#include <algorithm>
#include <cstring>

namespace hpactor {
namespace net {

// =============================================================================
// RouteRegistry Implementation
// =============================================================================

static std::vector<PatternSegment> parse_pattern(const std::string& pattern) {
    std::vector<PatternSegment> segments;
    if (pattern.empty() || pattern[0] != '/')
        return segments;

    size_t pos = 1;
    while (pos < pattern.size()) {
        size_t end = pattern.find('/', pos);
        std::string seg =
            pattern.substr(pos, end == std::string::npos ? end : end - pos);
        if (seg.empty()) {
            pos = end == std::string::npos ? pattern.size() : end + 1;
            continue;
        }
        if (seg[0] == '*') {
            if (seg.size() > 1) {
                segments.push_back(
                    {PatternSegmentType::MultiWildcard, seg.substr(1)});
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

    size_t pi = 0;
    size_t si = 0;
    while (si < segments.size() && pi < path_segs.size()) {
        const auto& seg = segments[si];
        switch (seg.type) {
            case PatternSegmentType::Literal:
                if (path_segs[pi] != seg.name)
                    return false;
                ++pi;
                ++si;
                break;
            case PatternSegmentType::NamedParam:
                req.path_params[seg.name] = path_segs[pi];
                ++pi;
                ++si;
                break;
            case PatternSegmentType::SingleWildcard:
                ++pi;
                ++si;
                break;
            case PatternSegmentType::MultiWildcard: {
                std::string remaining;
                for (size_t i = pi; i < path_segs.size(); ++i) {
                    if (i > pi)
                        remaining += '/';
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
    routes_.push_back({method, parse_pattern(pattern), std::move(builder), priority});
    std::sort(routes_.begin(), routes_.end(), [](const Route& a, const Route& b) {
        return a.priority < b.priority;
    });
}

const RouteRegistry::MessageBuilder*
RouteRegistry::match(HttpMethod method, const std::string& path,
                     HttpRequest& req) const {
    for (const auto& route : routes_) {
        if (route.method != method)
            continue;
        if (match_pattern(route.segments, path, req)) {
            return &route.builder;
        }
    }
    return nullptr;
}

// =============================================================================
// HTTPGateway Implementation
// =============================================================================

HTTPGateway::HTTPGateway() : own_loop_(std::make_unique<EventLoop>()) {
    loop_ = own_loop_.get();
}

HTTPGateway::HTTPGateway(EventLoop* loop) : loop_(loop) {
    if (!loop_) {
        own_loop_ = std::make_unique<EventLoop>();
        loop_ = own_loop_.get();
    }
}

HTTPGateway::~HTTPGateway() {
    stop();
}

bool HTTPGateway::listen(uint16_t port, const std::string& bind_host) {
    bind_host_ = bind_host;
    port_ = port;

    // If we own the event loop, start it now.
    if (own_loop_ && !loop_->is_running()) {
        loop_->run();
    }

    acceptor_ = std::make_unique<TcpAcceptor>(loop_);
    acceptor_->set_accept_handler([this](int client_fd, EndPoint remote_endpoint) {
        on_accept(client_fd, remote_endpoint);
    });

    if (!acceptor_->listen(port_, 0, bind_host_)) {
        acceptor_.reset();
        return false;
    }
    return true;
}

void HTTPGateway::stop() {
    {
        std::lock_guard<std::mutex> lock(conn_mutex_);
        for (auto& conn : connections_) {
            conn->close();
        }
        connections_.clear();
    }
    if (acceptor_) {
        acceptor_->close();
    }
    // Only stop the event loop if we own it.
    if (own_loop_) {
        loop_->stop();
    }
}

bool HTTPGateway::is_listening() const {
    return acceptor_ && acceptor_->is_listening();
}

uint16_t HTTPGateway::port() const {
    return acceptor_ ? acceptor_->port() : port_;
}

void HTTPGateway::run_once() {
    loop_->wait(100);
    loop_->process_completions();
}

void HTTPGateway::set_request_handler(RequestHandler handler) {
    request_handler_ = std::move(handler);
}

void HTTPGateway::set_error_handler(ErrorHandler handler) {
    error_handler_ = std::move(handler);
}

void HTTPGateway::send_response(HTTPConnection* conn, HttpStatusCode code,
                                std::vector<HttpHeader> headers, StreamBuffer body) {
    conn->send_response(code, std::move(headers), std::move(body));
}

void HTTPGateway::close_connection(HTTPConnection* conn) {
    std::lock_guard<std::mutex> lock(conn_mutex_);
    auto it = std::find_if(
        connections_.begin(), connections_.end(),
        [conn](const HTTPConnectionPtr& p) { return p.get() == conn; });
    if (it != connections_.end()) {
        (*it)->close();
        connections_.erase(it);
    }
}

void HTTPGateway::set_max_connections(size_t max) {
    max_connections_ = max;
}

void HTTPGateway::set_max_request_size(size_t max) {
    max_request_size_ = max;
}

void HTTPGateway::on_accept(int client_fd, EndPoint remote_endpoint) {
    {
        std::lock_guard<std::mutex> lock(conn_mutex_);
        if (connections_.size() >= max_connections_) {
            ::close(client_fd);
            return;
        }
    }

    auto conn = HTTPConnection::create(client_fd, LocalEndpoint, remote_endpoint,
                                       loop_, HTTPConnectionMode::Server);

    conn->set_request_handler([this](HTTPConnection* c, HttpRequest&& req) {
        if (request_handler_) {
            request_handler_(c, std::move(req));
        }
    });
    conn->set_error_handler([this](HTTPConnection* c, const error& err) {
        if (error_handler_) {
            error_handler_(c, err);
        }
    });

    {
        std::lock_guard<std::mutex> lock(conn_mutex_);
        connections_.push_back(std::move(conn));
    }
}

} // namespace net
} // namespace hpactor
