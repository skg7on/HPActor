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
/// Connects via Unix domain socket (default) or TCP, sends commands
/// and receives NUL-terminated responses.  Supports single-command
/// (--exec) and interactive (readline) modes.

#include <hpactor/cli/line_editor.hpp>

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
// Connection helpers
// ---------------------------------------------------------------------------

/// Open a blocking Unix domain stream socket connected to \p path.
static int connect_uds(const std::string& path) {
    int fd = static_cast<int>(::socket(AF_UNIX, SOCK_STREAM, 0));
    if (fd < 0) {
        std::cerr << "Error: cannot create UDS socket: " << std::strerror(errno)
                  << "\n";
        return -1;
    }

    struct sockaddr_un addr{};
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "Error: cannot connect to " << path << ": "
                  << std::strerror(errno) << "\n";
        ::close(fd);
        return -1;
    }
    return fd;
}

/// Open a blocking TCP stream socket connected to \p host:\p port.
static int connect_tcp(const std::string& host, uint16_t port) {
    int fd = static_cast<int>(::socket(AF_INET, SOCK_STREAM, 0));
    if (fd < 0) {
        std::cerr << "Error: cannot create TCP socket: " << std::strerror(errno)
                  << "\n";
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

    if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
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
// Protocol helpers
//
// Wire format:
//   Client -> Server:  <command-line>\n
//   Server -> Client:  <response-text>\0
// ---------------------------------------------------------------------------

/// Send a single command line to the server.
static bool send_line(int fd, const std::string& line) {
    std::string payload = line + "\n";
    return ::write(fd, payload.data(), payload.size()) >= 0;
}

/// Read one response from the server, delimited by a NUL byte.
///
/// Accumulates data across multiple reads to handle fragmentation.
/// Strips the trailing NUL and returns the preceding text.
///
/// \param[in]  fd   Connected socket.
/// \param[out] buf  Persistent buffer that stores partial reads between
///                  calls.
/// \return The response text without the terminating NUL, or empty on
///         connection close / error.
static std::string read_response(int fd, std::string& buf) {
    // If a complete response is already buffered, extract it first.
    auto nul = buf.find('\0');
    if (nul != std::string::npos) {
        std::string result = buf.substr(0, nul);
        buf.erase(0, nul + 1);
        return result;
    }

    char read_buf[4096];
    while (true) {
        ssize_t n = ::read(fd, read_buf, sizeof(read_buf));
        if (n <= 0)
            break; // EOF or error

        buf.append(read_buf, static_cast<size_t>(n));

        nul = buf.find('\0');
        if (nul != std::string::npos) {
            std::string result = buf.substr(0, nul);
            buf.erase(0, nul + 1);
            return result;
        }
    }

    // Connection closed — flush whatever remains in the buffer.
    if (!buf.empty()) {
        std::string result = std::move(buf);
        buf.clear();
        return result;
    }
    return {};
}

// ---------------------------------------------------------------------------
// Usage
// ---------------------------------------------------------------------------

static void print_usage(const char* prog) {
    // NOTE: keep in sync with synopsis at the top of main().
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
    // Format hint is parsed but not sent over the wire; the user can set
    // format inside interactive mode or embed it in the exec command.
    // std::string format = "pretty";
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
            // format = argv[++i]; -- stored but unused (see above).
            ++i;
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

    // --- connect ---
    int raw_fd;
    if (!use_uds && !host.empty()) {
        if (port == 0) {
            std::cerr << "Error: TCP mode requires --port\n";
            return 1;
        }
        raw_fd = connect_tcp(host, port);
    } else {
        raw_fd = connect_uds(socket_path);
    }
    if (raw_fd < 0)
        return 1;

    FdGuard guard{raw_fd};
    int fd = guard.fd;

    // ---- exec mode (single command, print response, exit) ----
    if (!exec_cmd.empty()) {
        send_line(fd, exec_cmd);
        std::string buf;
        auto response = read_response(fd, buf);
        std::cout << response;
        return 0;
    }

    // ---- interactive mode (readline loop) ----
    std::string buf;
    auto greeting = read_response(fd, buf);
    if (!greeting.empty())
        std::cout << greeting << std::flush;
    // else: empty greeting is not an error; the server may send nothing
    // before the first command for non-standard configurations.

    using namespace hpactor::cli;
    LineEditor editor(
        LineEditorConfig{default_history_path(), 1000, /*multiline=*/false},
        /*root=*/nullptr);
    editor.load_history();

    while (true) {
        auto line = editor.readline("> ");
        if (line.empty()) // EOF (Ctrl-D)
            break;

        editor.add_history(line);

        if (!send_line(fd, line)) {
            std::cerr << "\n[Error: connection lost]\n";
            break;
        }

        auto response = read_response(fd, buf);

        if (line == "/quit") {
            std::cout << "Goodbye.\n";
            break;
        }

        if (response.empty() && buf.empty()) {
            // Both the response and the buffer are empty, which means
            // the server closed the connection without sending a NUL.
            std::cerr << "\n[Connection closed]\n";
            break;
        }

        std::cout << response << std::flush;
    }

    editor.save_history();
    return 0;
}
