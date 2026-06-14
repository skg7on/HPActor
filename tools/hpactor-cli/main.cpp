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

/// \file main.cpp
/// \brief Standalone CLI client for the hpactor daemon.
///
/// Connects via Unix domain socket (default) or TCP using the HPActor
/// EventLoop for all network I/O: non-blocking connect with write-handler
/// completion, async send/recv, and fd-readiness polling.
///
/// Wire protocol:
///   Client -> Server:  <command-line>\n
///   Server -> Client:  <response-text>\0

#include <hpactor/cli/line_editor.hpp>
#include <hpactor/net/event_loop.hpp>

#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// Connection helpers — create non-blocking socket, initiate async connect
// ---------------------------------------------------------------------------

/// Set O_NONBLOCK and FD_CLOEXEC on a socket fd.
/// Portable alternative to Linux-only SOCK_NONBLOCK | SOCK_CLOEXEC.
static bool make_nonblocking(int fd) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return false;
    if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
        return false;

    flags = ::fcntl(fd, F_GETFD, 0);
    if (flags < 0)
        return false;
    if (::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0)
        return false;

    return true;
}

/// Create a non-blocking Unix domain socket and initiate connection.
/// Returns the fd on success, or -1 on error.
static int connect_uds_nonblock(const std::string& path) {
    int fd = static_cast<int>(::socket(AF_UNIX, SOCK_STREAM, 0));
    if (fd < 0) {
        std::cerr << "Error: cannot create UDS socket: " << std::strerror(errno)
                  << "\n";
        return -1;
    }
    if (!make_nonblocking(fd)) {
        std::cerr << "Error: fcntl failed: " << std::strerror(errno) << "\n";
        ::close(fd);
        return -1;
    }

    struct sockaddr_un addr{};
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    int ret =
        ::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    if (ret < 0 && errno != EINPROGRESS) {
        std::cerr << "Error: cannot connect to " << path << ": "
                  << std::strerror(errno) << "\n";
        ::close(fd);
        return -1;
    }
    return fd;
}

/// Create a non-blocking TCP socket and initiate connection.
/// Returns the fd on success, or -1 on error.
static int connect_tcp_nonblock(const std::string& host, uint16_t port) {
    int fd = static_cast<int>(::socket(AF_INET, SOCK_STREAM, 0));
    if (fd < 0) {
        std::cerr << "Error: cannot create TCP socket: " << std::strerror(errno)
                  << "\n";
        return -1;
    }
    if (!make_nonblocking(fd)) {
        std::cerr << "Error: fcntl failed: " << std::strerror(errno) << "\n";
        ::close(fd);
        return -1;
    }

    struct sockaddr_in addr{};
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        std::cerr << "Error: invalid address: " << host << "\n";
        ::close(fd);
        return -1;
    }

    int ret =
        ::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    if (ret < 0 && errno != EINPROGRESS) {
        std::cerr << "Error: cannot connect to " << host << ":" << port << ": "
                  << std::strerror(errno) << "\n";
        ::close(fd);
        return -1;
    }
    return fd;
}

/// RAII wrapper that closes a file descriptor on scope exit.
struct FdGuard {
    int fd;
    ~FdGuard() {
        if (fd >= 0)
            ::close(fd);
    }
};

// ---------------------------------------------------------------------------
// EventLoop-based async connect — waits for non-blocking connect to complete
// ---------------------------------------------------------------------------

/// Wait for a non-blocking connect to complete using the EventLoop's
/// write-handler mechanism.  The write handler fires when the socket becomes
/// writable, which signals connect completion.
///
/// \returns true on successful connect, false on failure (SO_ERROR != 0).
static bool await_connect(hpactor::net::EventLoop& loop, int fd) {
    bool connected = false;
    int connect_error = 0;

    loop.set_write_handler(fd, [&](int /*write_fd*/) {
        socklen_t len = sizeof(connect_error);
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &connect_error, &len) < 0) {
            connect_error = errno;
        }
        connected = true;
    });

    // Poll until the write handler fires or we time out.
    constexpr int kConnectTimeoutMs = 5000;
    int elapsed = 0;
    while (!connected && elapsed < kConnectTimeoutMs) {
        loop.wait(50); // 50 ms polling interval
        elapsed += 50;
    }

    loop.clear_write_handler(fd);

    if (!connected) {
        std::cerr << "Error: connect timed out after " << kConnectTimeoutMs
                  << "ms\n";
        return false;
    }
    if (connect_error != 0) {
        std::cerr << "Error: connect failed: " << std::strerror(connect_error)
                  << "\n";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Protocol helpers
// ---------------------------------------------------------------------------

/// Send a single command line using async send via the event loop backend.
static void
send_line_async(hpactor::net::EventLoop& loop, int fd, const std::string& line) {
    std::string payload = line + "\n";
    struct iovec iov;
    iov.iov_base = const_cast<char*>(payload.data());
    iov.iov_len = payload.size();

    // Use a dummy ActorId (0) — the CLI client is not an actor.
    loop.backend()->async_send(fd, &iov, 1, hpactor::ActorId{}, 0);

    // Give the backend a chance to process the send.
    loop.wait(10);
}

/// Read response data into the persistent buffer, driven by the EventLoop's
/// read handler.  Returns when a complete NUL-terminated response is received
/// or the connection closes.
///
/// \param[in]  loop       EventLoop that monitors the fd.
/// \param[in]  fd         Connected socket.
/// \param[out] buf        Persistent buffer for partial reads.
/// \param[out] response   Set to the complete response text (no NUL) on return.
/// \param[out] eof        Set to true if the connection was closed.
static void
recv_response_async(hpactor::net::EventLoop& loop, int fd, std::string& buf,
                    std::string& response, bool& eof) {
    eof = false;
    response.clear();

    // Check if a complete response is already buffered.
    auto nul = buf.find('\0');
    if (nul != std::string::npos) {
        response = buf.substr(0, nul);
        buf.erase(0, nul + 1);
        return;
    }

    // Install a read handler that drains the socket into buf.
    bool read_done = false;
    loop.set_read_handler(fd, [&](int /*read_fd*/) {
        char read_buf[4096];
        ssize_t n = ::read(fd, read_buf, sizeof(read_buf));
        if (n > 0) {
            buf.append(read_buf, static_cast<size_t>(n));
            if (buf.find('\0') != std::string::npos) {
                read_done = true;
            }
        } else if (n == 0) {
            // EOF — server closed connection.
            eof = true;
            read_done = true;
        }
        // n < 0: EAGAIN on non-blocking socket, just try again next poll.
    });

    // Poll until we have a complete response, EOF, or timeout.
    constexpr int kResponseTimeoutMs = 10000;
    int elapsed = 0;
    while (!read_done && elapsed < kResponseTimeoutMs) {
        loop.wait(20);
        elapsed += 20;
    }

    loop.clear_read_handler(fd);

    // Extract the completed response from the buffer.
    nul = buf.find('\0');
    if (nul != std::string::npos) {
        response = buf.substr(0, nul);
        buf.erase(0, nul + 1);
    } else if (!buf.empty() && eof) {
        // Connection closed mid-response — flush whatever we have.
        response = std::move(buf);
        buf.clear();
    }
}

// ---------------------------------------------------------------------------
// Usage
// ---------------------------------------------------------------------------

static void print_usage(const char* prog) {
    std::cerr
        << "Usage: " << prog << " [options]\n"
        << "Options:\n"
        << "  -s, --socket PATH   Unix domain socket path\n"
        << "                      (default: /var/run/hpactor/hpactor.sock)\n"
        << "  -H, --host HOST     TCP host address (disables UDS)\n"
        << "  -p, --port PORT     TCP port (required with --host)\n"
        << "  -e, --exec CMD      Execute a single command and exit\n"
        << "  -f, --format FMT    Output format hint (pretty, json, tabular)\n"
        << "  -h, --help          Show this help message\n";
}

static std::string default_history_path() {
    const char* home = std::getenv("HOME");
    return (home ? std::string(home) : "/tmp") + "/.hpactor_cli_history";
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    // --- defaults ---
    std::string socket_path = "/var/run/hpactor/hpactor.sock";
    std::string host;
    uint16_t port = 0;
    std::string exec_cmd;
    bool use_uds = true;
    bool show_help = false;

    // --- argument parsing ---
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-s" || arg == "--socket") {
            if (i + 1 >= argc) {
                std::cerr << "Error: --socket requires a path argument\n";
                return 1;
            }
            socket_path = argv[++i];
            use_uds = true;
        } else if (arg == "-H" || arg == "--host") {
            if (i + 1 >= argc) {
                std::cerr << "Error: --host requires a host argument\n";
                return 1;
            }
            host = argv[++i];
            use_uds = false;
        } else if (arg == "-p" || arg == "--port") {
            if (i + 1 >= argc) {
                std::cerr << "Error: --port requires a port argument\n";
                return 1;
            }
            port = static_cast<uint16_t>(std::atoi(argv[++i]));
        } else if (arg == "-e" || arg == "--exec") {
            if (i + 1 >= argc) {
                std::cerr << "Error: --exec requires a command argument\n";
                return 1;
            }
            exec_cmd = argv[++i];
        } else if (arg == "-f" || arg == "--format") {
            if (i + 1 >= argc) {
                std::cerr << "Error: --format requires a format argument\n";
                return 1;
            }
            ++i; // stored but unused (sent as part of exec command).
        } else if (arg == "-h" || arg == "--help") {
            show_help = true;
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
            print_usage(argv[0]);
            return 1;
        }
    }

    if (show_help) {
        print_usage(argv[0]);
        return 0;
    }

    // --- create EventLoop (owns the platform I/O backend) ---
    hpactor::net::EventLoop loop;

    // --- create non-blocking socket ---
    int fd;
    if (!use_uds && !host.empty()) {
        if (port == 0) {
            std::cerr << "Error: TCP mode requires --port\n";
            return 1;
        }
        fd = connect_tcp_nonblock(host, port);
    } else {
        fd = connect_uds_nonblock(socket_path);
    }
    if (fd < 0)
        return 1;

    FdGuard guard{fd};

    // --- async connect via EventLoop write handler ---
    if (!await_connect(loop, fd))
        return 1;

    // --- exec mode (single command, print response, exit) ---
    if (!exec_cmd.empty()) {
        send_line_async(loop, fd, exec_cmd);

        std::string buf;
        std::string response;
        bool eof = false;
        recv_response_async(loop, fd, buf, response, eof);
        std::cout << response;
        return 0;
    }

    // --- interactive mode (readline + event loop for network) ---
    using namespace hpactor::cli;
    LineEditor editor(
        LineEditorConfig{default_history_path(), 1000, /*multiline=*/false},
        /*root=*/nullptr);
    editor.load_history();

    // Read the server greeting (arrives on connect).
    {
        std::string buf;
        std::string greeting;
        bool eof = false;
        recv_response_async(loop, fd, buf, greeting, eof);
        if (!greeting.empty())
            std::cout << greeting << std::flush;
    }

    std::string recv_buf; // persistent buffer across read_response calls

    while (true) {
        auto line = editor.readline("> ");
        if (line.empty())
            break; // EOF (Ctrl-D)

        editor.add_history(line);

        // Send the command line via async send.
        send_line_async(loop, fd, line);

        // Receive the NUL-terminated response via event loop read handler.
        bool eof = false;
        std::string response;
        recv_response_async(loop, fd, recv_buf, response, eof);

        if (line == "/quit") {
            std::cout << "Goodbye.\n";
            break;
        }

        if (response.empty() && eof) {
            std::cerr << "\n[Connection closed]\n";
            break;
        }

        std::cout << response << std::flush;
    }

    editor.save_history();
    return 0;
}
