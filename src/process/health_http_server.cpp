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
#include <hpactor/net/event_loop.hpp>
#include <hpactor/net/http_connection.hpp>
#include <hpactor/net/http_types.hpp>
#include <hpactor/process/health_http_server.hpp>

#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <string>

namespace hpactor::process {

HealthHttpServer::HealthHttpServer(ActorContext* ctx, ActorSystem& system,
                                   const HealthHttpConfig& config)
    : DaemonActor(ctx, system), system_(system), config_(config),
      health_loop_(std::make_unique<net::EventLoop>()) {}

HealthHttpServer::~HealthHttpServer() = default;

void HealthHttpServer::on_daemon_start() {
    // Create non-blocking listen socket.
    listen_fd_ = static_cast<int>(::socket(AF_INET, SOCK_STREAM, 0));
    if (listen_fd_ < 0) {
        std::fprintf(stderr, "HealthHttpServer: socket creation failed: %s\n",
                     std::strerror(errno));
        running_ = false;
        return;
    }

    // Non-blocking + close-on-exec.
    int flags = ::fcntl(listen_fd_, F_GETFL, 0);
    if (flags >= 0)
        ::fcntl(listen_fd_, F_SETFL, flags | O_NONBLOCK);
    flags = ::fcntl(listen_fd_, F_GETFD, 0);
    if (flags >= 0)
        ::fcntl(listen_fd_, F_SETFD, flags | FD_CLOEXEC);

    int reuse = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr{};
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(config_.port);
    if (::inet_pton(AF_INET, config_.bind_address.c_str(), &addr.sin_addr) != 1) {
        std::fprintf(stderr, "HealthHttpServer: invalid bind address: %s\n",
                     config_.bind_address.c_str());
        ::close(listen_fd_);
        listen_fd_ = -1;
        running_ = false;
        return;
    }

    if (::bind(listen_fd_, reinterpret_cast<struct sockaddr*>(&addr),
               sizeof(addr)) < 0) {
        std::fprintf(stderr, "HealthHttpServer: bind failed on port %u: %s\n",
                     static_cast<unsigned>(config_.port), std::strerror(errno));
        ::close(listen_fd_);
        listen_fd_ = -1;
        running_ = false;
        return;
    }

    if (::listen(listen_fd_, 8) < 0) {
        std::fprintf(stderr, "HealthHttpServer: listen failed: %s\n",
                     std::strerror(errno));
        ::close(listen_fd_);
        listen_fd_ = -1;
        running_ = false;
        return;
    }

    // Register the listen fd with the EventLoop.
    health_loop_->set_read_handler(listen_fd_,
                                   [this](int fd) { on_listen_readable(fd); });
}

void HealthHttpServer::on_listen_readable(int listen_fd) {
    // Drain all pending connections.
    while (true) {
        int client_fd =
#ifdef __linux__
            static_cast<int>(::accept4(listen_fd, nullptr, nullptr,
                                       SOCK_NONBLOCK | SOCK_CLOEXEC));
#else
            static_cast<int>(::accept(listen_fd, nullptr, nullptr));
#endif
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            break;
        }

#ifndef __linux__
        int fl = ::fcntl(client_fd, F_GETFL, 0);
        if (fl >= 0)
            ::fcntl(client_fd, F_SETFL, fl | O_NONBLOCK);
        fl = ::fcntl(client_fd, F_GETFD, 0);
        if (fl >= 0)
            ::fcntl(client_fd, F_SETFD, fl | FD_CLOEXEC);
#endif

        // Create an HTTPConnection (Server mode) for this client.
        // It registers itself with the EventLoop and handles all HTTP
        // protocol details: parsing via llhttp, response formatting,
        // and async send/recv via the backend.
        //
        // Use loopback endpoints — health checks are internal and don't
        // need accurate endpoint tracking.
        // Default-constructed EndPoint is loopback — fine for internal
        // health checks that don't need accurate endpoint tracking.
        EndPoint local_ep{};
        EndPoint remote_ep{};

        auto conn = net::HTTPConnection::create(client_fd, local_ep, remote_ep,
                                                health_loop_.get(),
                                                net::HTTPConnectionMode::Server);

        // Install the request handler: all paths return 200 OK.
        // The HTTPConnection already parsed the request via llhttp, so
        // req.path is exactly the URL path.
        conn->set_request_handler([this](net::HTTPConnection* c,
                                         net::HttpRequest&& req) {
            std::string body_str = health_body(req.path);
            StreamBuffer body_buf(
                reinterpret_cast<const uint8_t*>(body_str.data()),
                reinterpret_cast<const uint8_t*>(body_str.data() + body_str.size()));
            c->send_response(net::HttpStatusCode::OK, {}, body_buf);
        });

        // On error or connection close, the connection self-cleans via
        // its destructor (called when shared_ptr refcount drops to zero).
        // We just need to reap dead entries from our vector periodically.

        connections_.push_back(std::move(conn));
    }
}

std::string HealthHttpServer::health_body(const std::string& path) const {
    (void)system_;
    (void)path;
    return "OK";
}

bool HealthHttpServer::run_once() {
    if (!running_)
        return false;

    // Drive the EventLoop: wait for readable/writable fds with a short
    // timeout, then process any completed async operations (send completions
    // from HTTPConnection::flush_write_buffer via the backend).
    health_loop_->wait(100);
    health_loop_->process_completions();

    // Clean up disconnected / errored connections.
    reap_connections();

    return running_;
}

void HealthHttpServer::reap_connections() {
    connections_.erase(
        std::remove_if(connections_.begin(), connections_.end(),
                       [](const net::HTTPConnectionPtr& conn) {
                           return !conn ||
                                  conn->state() ==
                                      net::ConnectionState::Disconnected ||
                                  conn->state() == net::ConnectionState::Error;
                       }),
        connections_.end());
}

void HealthHttpServer::on_daemon_stop() {
    running_ = false;
    if (listen_fd_ >= 0) {
        health_loop_->clear_read_handler(listen_fd_);
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
    // Close all active connections (HTTPConnection::close() deregisters
    // fds from the EventLoop).
    for (auto& conn : connections_) {
        conn->close();
    }
    connections_.clear();
}

} // namespace hpactor::process
