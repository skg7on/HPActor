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

    // Non-blocking + close-on-exec (portable — no SOCK_NONBLOCK on macOS).
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

    // Register the listen fd with the EventLoop.  The read_handler is
    // invoked when the fd becomes readable (new connections pending).
    health_loop_->set_read_handler(listen_fd_,
                                   [this](int fd) { on_listen_readable(fd); });
}

void HealthHttpServer::on_listen_readable(int listen_fd) {
    // Drain all pending connections — the listen fd is non-blocking,
    // so accept() returns -1 / EAGAIN when no more are ready.
    while (true) {
        int client_fd =
#ifdef __linux__
            static_cast<int>(::accept4(listen_fd, nullptr, nullptr,
                                       SOCK_NONBLOCK | SOCK_CLOEXEC));
#else
            static_cast<int>(::accept(listen_fd, nullptr, nullptr));
#endif
        if (client_fd < 0) {
            // EAGAIN / EWOULDBLOCK — no more pending connections.
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            // Real error — log and keep going.
            break;
        }

#ifndef __linux__
        // macOS: set non-blocking + cloexec on the accepted fd.
        int fl = ::fcntl(client_fd, F_GETFL, 0);
        if (fl >= 0)
            ::fcntl(client_fd, F_SETFL, fl | O_NONBLOCK);
        fl = ::fcntl(client_fd, F_GETFD, 0);
        if (fl >= 0)
            ::fcntl(client_fd, F_SETFD, fl | FD_CLOEXEC);
#endif

        // Read the HTTP request.  The request is tiny for health checks
        // and should be fully buffered in the kernel by the time we get
        // here, so a single non-blocking read() normally suffices.
        char buf[2048];
        ssize_t n = ::read(client_fd, buf, sizeof(buf) - 1);
        if (n <= 0) {
            ::close(client_fd);
            continue;
        }
        buf[n] = '\0';

        // Parse the HTTP request line for the path.
        std::string request(buf);
        std::string path = "/";
        auto first_space = request.find(' ');
        if (first_space != std::string::npos) {
            auto second_space = request.find(' ', first_space + 1);
            if (second_space != std::string::npos) {
                path = request.substr(first_space + 1,
                                      second_space - first_space - 1);
            }
        }

        std::string body = health_response(path);
        std::string response = "HTTP/1.1 200 OK\r\n"
                               "Content-Type: text/plain\r\n"
                               "Content-Length: " +
                               std::to_string(body.size()) +
                               "\r\n"
                               "Connection: close\r\n"
                               "\r\n" +
                               body;

        // Write response.  For a tiny health-check body this fits in the
        // kernel buffer on the first call.  Retry once on partial write.
        ssize_t written = ::write(client_fd, response.data(), response.size());
        if (written >= 0 && static_cast<size_t>(written) < response.size()) {
            // Partial write — retry the remainder (best-effort).
            ssize_t remain = static_cast<ssize_t>(response.size()) - written;
            ::write(client_fd, response.data() + written,
                    static_cast<size_t>(remain));
        }
        ::close(client_fd);
    }
}

bool HealthHttpServer::run_once() {
    if (!running_)
        return false;

    // Let the EventLoop process pending I/O events with a short timeout.
    // On return the read_handler may have been invoked zero or more times.
    health_loop_->wait(100); // 100 ms timeout

    return running_;
}

std::string HealthHttpServer::health_response(const std::string& /*path*/) const {
    (void)system_;
    return "OK";
}

void HealthHttpServer::on_daemon_stop() {
    running_ = false;
    if (listen_fd_ >= 0) {
        health_loop_->clear_read_handler(listen_fd_);
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
}

} // namespace hpactor::process
