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

#include <hpactor/cli/cli_connector.hpp>
#include <hpactor/net/event_loop.hpp>

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace hpactor {
namespace cli {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// Set O_NONBLOCK on a socket fd.  Returns true on success.
static bool set_nonblock(int fd) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return false;
    return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) >= 0;
}

/// Complete a non-blocking connect on @p fd using a temporary EventLoop.
///
/// Registers a write_handler and a timeout; polls the loop until one fires.
/// Checks SO_ERROR to distinguish success from failure.
///
/// \returns true on successful connect, false on failure or timeout.
static bool await_async_connect(net::EventLoop& loop, int fd,
                                std::chrono::milliseconds timeout) {
    auto done = std::make_shared<bool>(false);
    auto success = std::make_shared<bool>(false);

    // Register write interest — the fd becomes writable when the TCP
    // handshake completes (or fails).
    loop.add_fd(fd, net::EventLoop::Event::Write);

    loop.set_write_handler(fd, [&loop, fd, done, success](int /*event_fd*/) {
        if (*done)
            return;
        *done = true;

        loop.clear_write_handler(fd);

        int so_error = 0;
        socklen_t len = sizeof(so_error);
        ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &len);

        *success = (so_error == 0);
    });

    // Timeout guard — prevents indefinite blocking if the remote is
    // unreachable (e.g. firewall drops SYN packets silently).
    loop.run_after(
        [&loop, fd, done]() {
            if (*done)
                return;
            *done = true;
            loop.clear_write_handler(fd);
        },
        static_cast<int>(timeout.count()));

    // Poll until the write_handler fires or the timeout expires.
    int elapsed = 0;
    constexpr int kPollIntervalMs = 50;
    while (!*done && elapsed < static_cast<int>(timeout.count())) {
        loop.wait(kPollIntervalMs);
        elapsed += kPollIntervalMs;
    }

    return *success;
}

// ---------------------------------------------------------------------------
// connect_tcp
// ---------------------------------------------------------------------------

int CliConnector::connect_tcp(const std::string& host, uint16_t port,
                              std::chrono::milliseconds timeout) {
    // Resolve host
    struct sockaddr_in addr{};
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        return -1;
    }

    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    // TCP_NODELAY — disable Nagle's algorithm for interactive latency.
    int nodelay = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    if (!set_nonblock(fd)) {
        ::close(fd);
        return -1;
    }

    int ret =
        ::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    if (ret == 0) {
        // Connected immediately (e.g. loopback).
        return fd;
    }
    if (errno != EINPROGRESS) {
        ::close(fd);
        return -1;
    }

    // Async connect via temporary EventLoop.
    net::EventLoop loop;
    loop.run();
    if (!await_async_connect(loop, fd, timeout)) {
        ::close(fd);
        return -1;
    }
    return fd;
}

// ---------------------------------------------------------------------------
// connect_uds
// ---------------------------------------------------------------------------

int CliConnector::connect_uds(const std::string& path,
                              std::chrono::milliseconds timeout) {
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    if (!set_nonblock(fd)) {
        ::close(fd);
        return -1;
    }

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    int ret =
        ::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    if (ret == 0) {
        // Connected immediately.
        return fd;
    }
    if (errno != EINPROGRESS) {
        ::close(fd);
        return -1;
    }

    // Async connect via temporary EventLoop.
    net::EventLoop loop;
    loop.run();
    if (!await_async_connect(loop, fd, timeout)) {
        ::close(fd);
        return -1;
    }
    return fd;
}

} // namespace cli
} // namespace hpactor
