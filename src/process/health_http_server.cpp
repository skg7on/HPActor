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
#include <hpactor/process/health_http_server.hpp>

#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <thread>

namespace hpactor::process {

HealthHttpServer::HealthHttpServer(ActorContext* ctx, ActorSystem& system,
                                   const HealthHttpConfig& config)
    : DaemonActor(ctx, system), system_(system), config_(config) {}

int HealthHttpServer::portable_accept(int listen_fd) {
#ifdef __linux__
    return static_cast<int>(::accept4(listen_fd, nullptr, nullptr, SOCK_CLOEXEC));
#else
    int fd = static_cast<int>(::accept(listen_fd, nullptr, nullptr));
    if (fd < 0)
        return -1;
    ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    return fd;
#endif
}

void HealthHttpServer::on_daemon_start() {
    listen_fd_ = static_cast<int>(::socket(AF_INET, SOCK_STREAM, 0));
    if (listen_fd_ < 0) {
        std::fprintf(stderr, "HealthHttpServer: socket creation failed: %s\n",
                     std::strerror(errno));
        running_ = false;
        return;
    }

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
}

bool HealthHttpServer::run_once() {
    if (!running_)
        return false;

    int client_fd = portable_accept(listen_fd_);
    if (client_fd >= 0) {
        handle_request(client_fd);
        ::close(client_fd);
    }

    // Yield to avoid busy-waiting
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    return running_;
}

void HealthHttpServer::handle_request(int client_fd) {
    char buf[2048];
    ssize_t n = ::read(client_fd, buf, sizeof(buf) - 1);
    if (n <= 0)
        return;
    buf[n] = '\0';

    // Parse the HTTP request line for the path
    std::string request(buf);
    std::string path = "/";
    auto first_space = request.find(' ');
    if (first_space != std::string::npos) {
        auto second_space = request.find(' ', first_space + 1);
        if (second_space != std::string::npos) {
            path = request.substr(first_space + 1, second_space - first_space - 1);
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

    ::write(client_fd, response.data(), response.size());
}

std::string HealthHttpServer::health_response(const std::string& /*path*/) const {
    (void)system_;
    return "OK";
}

void HealthHttpServer::on_daemon_stop() {
    running_ = false;
    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
}

} // namespace hpactor::process
