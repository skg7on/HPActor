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

#include "commands/ask_commands.hpp"
#include <hpactor/cli/cli_server_actor.hpp>
#include <hpactor/cli/cli_session.hpp>
#include <hpactor/cli/command_context.hpp>
#include <hpactor/cli/command_node.hpp>
#include <hpactor/cli/command_registry.hpp>
#include <hpactor/cli/command_tree_builder.hpp>
#include <hpactor/cli/output_formatter.hpp>
#include <hpactor/core/actor_system.hpp>

#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <grp.h>
#include <netinet/in.h>
#include <pwd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <thread>

namespace hpactor {
namespace cli {

// ---------------------------------------------------------------------------
// Portability helpers
// ---------------------------------------------------------------------------

int CliServerActor::make_nonblocking_socket(int domain, int type) {
    int fd = static_cast<int>(::socket(domain, type, 0));
    if (fd < 0)
        return -1;

    // Set close-on-exec
    if (::fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
        std::fprintf(stderr, "CliServerActor: fcntl F_SETFD failed: %s\n",
                     std::strerror(errno));
        ::close(fd);
        return -1;
    }

    // Set non-blocking
    int flags = ::fcntl(fd, F_GETFL);
    if (flags < 0) {
        std::fprintf(stderr, "CliServerActor: fcntl F_GETFL failed: %s\n",
                     std::strerror(errno));
        ::close(fd);
        return -1;
    }
    if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        std::fprintf(stderr, "CliServerActor: fcntl F_SETFL failed: %s\n",
                     std::strerror(errno));
        ::close(fd);
        return -1;
    }

    return fd;
}

int CliServerActor::portable_accept(int listen_fd) {
#ifdef __linux__
    return static_cast<int>(
        ::accept4(listen_fd, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK));
#else
    int fd = static_cast<int>(::accept(listen_fd, nullptr, nullptr));
    if (fd < 0)
        return -1;
    if (::fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
        std::fprintf(stderr, "CliServerActor: fcntl F_SETFD failed: %s\n",
                     std::strerror(errno));
        ::close(fd);
        return -1;
    }
    int flags = ::fcntl(fd, F_GETFL);
    if (flags < 0) {
        std::fprintf(stderr, "CliServerActor: fcntl F_GETFL failed: %s\n",
                     std::strerror(errno));
        ::close(fd);
        return -1;
    }
    if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        std::fprintf(stderr, "CliServerActor: fcntl F_SETFL failed: %s\n",
                     std::strerror(errno));
        ::close(fd);
        return -1;
    }
    return fd;
#endif
}

/// \brief Write to a file descriptor avoiding SIGPIPE.
///
/// On platforms that define \c MSG_NOSIGNAL (Linux), uses \c send() with
/// that flag.  Otherwise falls back to \c write(); on macOS the caller
/// should set \c SO_NOSIGPIPE on the socket beforehand.
static ssize_t safe_write(int fd, const void* buf, size_t len) {
#ifdef MSG_NOSIGNAL
    return ::send(fd, buf, len, MSG_NOSIGNAL);
#else
    return ::write(fd, buf, len);
#endif
}

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

CliServerActor::CliServerActor(ActorContext* ctx, ActorSystem& system,
                               const CliServerConfig& config)
    : DaemonActor(ctx, system), system_(system), config_(config) {}

CliServerActor::~CliServerActor() = default;

// ---------------------------------------------------------------------------
// Bind listeners
// ---------------------------------------------------------------------------

result<void> CliServerActor::bind_listeners() {
    // --- Unix domain socket ---
    if (!config_.uds_listen_path.empty()) {
        uds_listen_fd_ = make_nonblocking_socket(AF_UNIX, SOCK_STREAM);
        if (uds_listen_fd_ < 0) {
            return result<void>::make(
                error(errors::unknown, "Failed to create UDS socket: " +
                                           std::string(std::strerror(errno))));
        }

        // Remove any existing socket file
        ::unlink(config_.uds_listen_path.c_str());

        struct sockaddr_un addr{};
        std::memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, config_.uds_listen_path.c_str(),
                     sizeof(addr.sun_path) - 1);

        if (::bind(uds_listen_fd_, reinterpret_cast<struct sockaddr*>(&addr),
                   sizeof(addr)) < 0) {
            ::close(uds_listen_fd_);
            uds_listen_fd_ = -1;
            return result<void>::make(
                error(errors::unknown, "Failed to bind UDS socket: " +
                                           std::string(std::strerror(errno))));
        }

        // Set permissions
        ::chmod(config_.uds_listen_path.c_str(),
                static_cast<mode_t>(config_.uds_socket_mode));

        // Set ownership if configured
        if (!config_.uds_socket_owner.empty() || !config_.uds_socket_group.empty()) {
            uid_t uid = static_cast<uid_t>(-1);
            gid_t gid = static_cast<gid_t>(-1);
            if (!config_.uds_socket_owner.empty()) {
                struct passwd* pw = ::getpwnam(config_.uds_socket_owner.c_str());
                if (pw != nullptr) {
                    uid = pw->pw_uid;
                } else {
                    std::fprintf(stderr, "CliServerActor: unknown UDS owner '%s'\n",
                                 config_.uds_socket_owner.c_str());
                }
            }
            if (!config_.uds_socket_group.empty()) {
                struct group* gr = ::getgrnam(config_.uds_socket_group.c_str());
                if (gr != nullptr) {
                    gid = gr->gr_gid;
                } else {
                    std::fprintf(stderr, "CliServerActor: unknown UDS group '%s'\n",
                                 config_.uds_socket_group.c_str());
                }
            }
            if (::chown(config_.uds_listen_path.c_str(), uid, gid) < 0) {
                std::fprintf(stderr, "CliServerActor: chown UDS socket failed: %s\n",
                             std::strerror(errno));
            }
        }

        if (::listen(uds_listen_fd_, 8) < 0) {
            ::close(uds_listen_fd_);
            uds_listen_fd_ = -1;
            return result<void>::make(
                error(errors::unknown, "Failed to listen on UDS socket: " +
                                           std::string(std::strerror(errno))));
        }
    }

    // --- TCP socket ---
    if (config_.tcp_listen_port > 0) {
        tcp_listen_fd_ = make_nonblocking_socket(AF_INET, SOCK_STREAM);
        if (tcp_listen_fd_ < 0) {
            return result<void>::make(
                error(errors::unknown, "Failed to create TCP socket: " +
                                           std::string(std::strerror(errno))));
        }

        int reuse = 1;
        ::setsockopt(tcp_listen_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse,
                     sizeof(reuse));

        struct sockaddr_in addr{};
        std::memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(config_.tcp_listen_port);
        if (::inet_pton(AF_INET, config_.tcp_bind_address.c_str(),
                        &addr.sin_addr) != 1) {
            ::close(tcp_listen_fd_);
            tcp_listen_fd_ = -1;
            return result<void>::make(
                error(errors::unknown,
                      "Invalid TCP bind address: " + config_.tcp_bind_address));
        }

        if (::bind(tcp_listen_fd_, reinterpret_cast<struct sockaddr*>(&addr),
                   sizeof(addr)) < 0) {
            ::close(tcp_listen_fd_);
            tcp_listen_fd_ = -1;
            return result<void>::make(
                error(errors::unknown, "Failed to bind TCP socket: " +
                                           std::string(std::strerror(errno))));
        }

        if (::listen(tcp_listen_fd_, 8) < 0) {
            ::close(tcp_listen_fd_);
            tcp_listen_fd_ = -1;
            return result<void>::make(
                error(errors::unknown, "Failed to listen on TCP socket: " +
                                           std::string(std::strerror(errno))));
        }
    }

    return result<void>::make();
}

// ---------------------------------------------------------------------------
// Daemon lifecycle
// ---------------------------------------------------------------------------

void CliServerActor::on_daemon_start() {
    auto res = bind_listeners();
    if (!res.ok()) {
        std::fprintf(stderr, "CliServerActor: bind failed: %s\n",
                     res.error().message().c_str());
        running_ = false;
        return;
    }
    build_command_tree();
}

void CliServerActor::on_daemon_stop() {
    // Close all active sessions
    for (auto& s : sessions_) {
        if (s.fd >= 0)
            ::close(s.fd);
    }
    sessions_.clear();

    // Close listeners
    if (uds_listen_fd_ >= 0) {
        ::close(uds_listen_fd_);
        uds_listen_fd_ = -1;
    }
    if (tcp_listen_fd_ >= 0) {
        ::close(tcp_listen_fd_);
        tcp_listen_fd_ = -1;
    }

    // Remove UDS socket file
    if (!config_.uds_listen_path.empty())
        ::unlink(config_.uds_listen_path.c_str());

    command_tree_.reset();
}

// ---------------------------------------------------------------------------
// Main daemon loop
// ---------------------------------------------------------------------------

bool CliServerActor::run_once() {
    if (!running_)
        return false;

    accept_connections();
    service_sessions();
    remove_dead_sessions();

    // Yield to avoid busy-waiting
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    return running_;
}

// ---------------------------------------------------------------------------
// Accept connections
// ---------------------------------------------------------------------------

void CliServerActor::accept_connections() {
    auto try_accept = [this](int listen_fd) {
        if (listen_fd < 0)
            return;

        int client_fd = portable_accept(listen_fd);
        if (client_fd < 0) {
            // EAGAIN / EWOULDBLOCK means no more connections right now
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return;
            // Other errors are logged but non-fatal
            std::fprintf(stderr, "CliServerActor: accept failed: %s\n",
                         std::strerror(errno));
            return;
        }

#ifdef __APPLE__
        // macOS: suppress SIGPIPE on writes to a client that has closed.
        int nosigpipe = 1;
        ::setsockopt(client_fd, SOL_SOCKET, SO_NOSIGPIPE, &nosigpipe,
                     sizeof(nosigpipe));
#endif

        // Check session limit
        if (sessions_.size() >= config_.max_sessions) {
            const char* msg =
                "HPActor CLI -- Too many connections. Try again later.\n";
            safe_write(client_fd, msg, std::strlen(msg));
            ::close(client_fd);
            return;
        }

        // Create CliSession and greeting
        auto formatter = OutputFormatter::create(config_.default_format);
        SessionState ss;
        ss.fd = client_fd;
        ss.last_activity = std::chrono::steady_clock::now();
        ss.session = std::make_unique<CliSession>(
            &system_, command_tree_.get(), std::move(formatter),
            [client_fd](const std::string& text) {
                safe_write(client_fd, text.data(), text.size());
                // Write null sentinel so client knows response is complete
                const char sentinel = '\0';
                safe_write(client_fd, &sentinel, 1);
            },
            config_.page_size);

        // Send greeting
        const char* greeting =
            "HPActor CLI -- Type /help for commands, /quit to exit.\n";
        safe_write(client_fd, greeting, std::strlen(greeting));
        const char sentinel = '\0';
        safe_write(client_fd, &sentinel, 1);

        sessions_.push_back(std::move(ss));
    };

    try_accept(uds_listen_fd_);
    try_accept(tcp_listen_fd_);
}

// ---------------------------------------------------------------------------
// Service existing sessions
// ---------------------------------------------------------------------------

void CliServerActor::service_sessions() {
    for (size_t i = 0; i < sessions_.size();) {
        auto& ss = sessions_[i];
        bool should_close = false;

        char buf[4096];
        ssize_t n = ::read(ss.fd, buf, sizeof(buf));

        if (n > 0) {
            ss.last_activity = std::chrono::steady_clock::now();
            ss.read_buffer.append(buf, static_cast<size_t>(n));

            // Process complete lines
            for (;;) {
                auto newline = ss.read_buffer.find('\n');
                if (newline == std::string::npos &&
                    ss.read_buffer.find('\r') == std::string::npos)
                    break;

                size_t end;
                if (newline != std::string::npos) {
                    end = newline;
                    // Handle \r\n
                    if (end > 0 && ss.read_buffer[end - 1] == '\r')
                        --end;
                } else {
                    end = ss.read_buffer.find('\r');
                }

                std::string line = ss.read_buffer.substr(0, end);

                // Skip past the newline(s)
                size_t skip = (newline != std::string::npos)
                                  ? newline + 1
                                  : ss.read_buffer.find('\r') + 1;
                ss.read_buffer.erase(0, skip);

                if (!ss.session->process_line(line)) {
                    should_close = true;
                    break;
                }
            }
        } else if (n == 0) {
            // Client disconnected
            should_close = true;
        } else {
            // EAGAIN / EWOULDBLOCK means no data right now
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                should_close = true;
            }
        }

        if (should_close) {
            close_session(i);
            // Do not increment i — the element was erased, next one
            // shifted into the same position.
        } else {
            ++i;
        }
    }
}

// ---------------------------------------------------------------------------
// Session management
// ---------------------------------------------------------------------------

void CliServerActor::remove_dead_sessions() {
    auto now = std::chrono::steady_clock::now();
    for (size_t i = 0; i < sessions_.size();) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - sessions_[i].last_activity);
        if (elapsed >= config_.session_timeout) {
            close_session(i);
        } else {
            ++i;
        }
    }
}

void CliServerActor::close_session(size_t index) {
    if (index >= sessions_.size())
        return;
    if (sessions_[index].fd >= 0)
        ::close(sessions_[index].fd);
    sessions_.erase(sessions_.begin() + static_cast<ptrdiff_t>(index));
}

// ---------------------------------------------------------------------------
// Command tree
// ---------------------------------------------------------------------------

void CliServerActor::build_command_tree() {
    auto root = std::make_unique<CommandNode>("/", "CLI root");
    build_command_tree_from_registry(*root);
    command_tree_ = std::move(root);
}

// ---------------------------------------------------------------------------
// Metadata
// ---------------------------------------------------------------------------

cli::ActorMeta CliServerActor::to_metadata() const {
    cli::ActorMeta m;
    m.actor_id = id().value();
    m.actor_type = std::string(type_name());
    m.state = running_ ? "Running" : "Stopped";
    return m;
}

} // namespace cli
} // namespace hpactor
