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

#include <hpactor/actor/actor_system.hpp>
#include <hpactor/cli/actor/cli_legacy_server_actor.hpp>
#include <hpactor/cli/command/cli_session.hpp>
#include <hpactor/cli/command/command_context.hpp>
#include <hpactor/cli/command/command_node.hpp>
#include <hpactor/cli/command/command_registry.hpp>
#include <hpactor/cli/command/command_tree_builder.hpp>
#include <hpactor/cli/format/output_formatter.hpp>
#include <hpactor/cli_messages.pb.h>
#include <hpactor/fault/fault_controller.hpp>
#include <hpactor/fault/fault_macros.hpp>
#include <hpactor/mailbox/dead_letter_queue.hpp>
#include <hpactor/mem/memory_region.hpp>
#include <hpactor/metrics/metrics_actor.hpp>
#include <hpactor/msg/dead_letter_record.hpp>
#include <hpactor/net/acceptor.hpp>
#include <hpactor/net/event_loop.hpp>
#include <hpactor/types/types.hpp>

#include "../commands/command_utils.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <grp.h>
#include <pwd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <unordered_map>

namespace hpactor {
namespace cli {

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

CliLegacyServerActor::CliLegacyServerActor(ActorContext* ctx, ActorSystem& system,
                                           const CliLegacyServerConfig& config)
    : DaemonActor(ctx, system), system_(system), config_(config),
      loop_(std::make_unique<net::EventLoop>()), host_impl_(system_) {}

CliLegacyServerActor::~CliLegacyServerActor() = default;

// ---------------------------------------------------------------------------
// DaemonActor interface
// ---------------------------------------------------------------------------

void CliLegacyServerActor::on_daemon_start() {
    build_command_tree();

    // --- Unix domain socket ---
    if (!config_.uds_listen_path.empty()) {
        // Remove stale socket file before binding.
        ::unlink(config_.uds_listen_path.c_str());

        uds_acceptor_ = std::make_unique<net::UnixDomainAcceptor>(loop_.get());
        uds_acceptor_->set_accept_handler(
            [this](int fd, EndPoint /*remote*/) { on_uds_accepted(fd); });

        if (!uds_acceptor_->listen(config_.uds_listen_path)) {
            std::fprintf(stderr, "CliLegacyServerActor: UDS listen failed on %s\n",
                         config_.uds_listen_path.c_str());
            uds_acceptor_.reset();
            running_ = false;
            return;
        }

        // Permissions and ownership.
        ::chmod(config_.uds_listen_path.c_str(),
                static_cast<mode_t>(config_.uds_socket_mode));
        if (!config_.uds_socket_owner.empty() || !config_.uds_socket_group.empty()) {
            uid_t uid = static_cast<uid_t>(-1);
            gid_t gid = static_cast<gid_t>(-1);
            if (!config_.uds_socket_owner.empty()) {
                struct passwd* pw = ::getpwnam(config_.uds_socket_owner.c_str());
                if (pw)
                    uid = pw->pw_uid;
            }
            if (!config_.uds_socket_group.empty()) {
                struct group* gr = ::getgrnam(config_.uds_socket_group.c_str());
                if (gr)
                    gid = gr->gr_gid;
            }
            ::chown(config_.uds_listen_path.c_str(), uid, gid);
        }
    }

    // --- TCP socket ---
    if (config_.tcp_listen_port > 0) {
        tcp_acceptor_ = std::make_unique<net::TcpAcceptor>(loop_.get());
        tcp_acceptor_->set_accept_handler([this](int fd, EndPoint remote_ep) {
            on_tcp_accepted(fd, remote_ep);
        });

        if (!tcp_acceptor_->listen(config_.tcp_listen_port, 0,
                                   config_.tcp_bind_address)) {
            std::fprintf(stderr,
                         "CliLegacyServerActor: TCP listen failed on %s:%u\n",
                         config_.tcp_bind_address.c_str(),
                         static_cast<unsigned>(config_.tcp_listen_port));
            tcp_acceptor_.reset();
            // UDS may still be working — don't abort.
        }
    }

    if (!uds_acceptor_ && !tcp_acceptor_) {
        std::fprintf(stderr, "CliLegacyServerActor: no listeners configured\n");
        running_ = false;
    }
}

void CliLegacyServerActor::on_daemon_stop() {
    running_ = false;

    // Close all client sessions (this also clears their read_handlers).
    for (auto& [fd, state] : sessions_) {
        loop_->clear_read_handler(fd);
        ::close(fd);
    }
    sessions_.clear();

    // Acceptors self-close on destruction.
    uds_acceptor_.reset();
    tcp_acceptor_.reset();

    if (!config_.uds_listen_path.empty())
        ::unlink(config_.uds_listen_path.c_str());

    command_tree_.reset();
}

bool CliLegacyServerActor::run_once() {
    if (!running_)
        return false;

    // Drive the EventLoop — same pattern as HTTPGateway::run_once().
    loop_->wait(100);

    return running_;
}

// ---------------------------------------------------------------------------
// Client connection management
// ---------------------------------------------------------------------------

void CliLegacyServerActor::on_tcp_accepted(int client_fd, EndPoint remote_ep) {
    if (sessions_.size() >= config_.max_sessions) {
        ::close(client_fd);
        return;
    }

    auto formatter = OutputFormatter::create(config_.default_format);
    auto session = std::make_unique<CliSession>(
        &system_, command_tree_.get(), std::move(formatter),
        [client_fd](const std::string& text) {
            // Best-effort write — the fd is non-blocking and may fail.
            ssize_t ignored = ::write(client_fd, text.data(), text.size());
            (void)ignored;
            const char sentinel = '\0';
            ignored = ::write(client_fd, &sentinel, 1);
            (void)ignored;
        },
        config_.page_size);
    session->set_cli_server_actor(this);
    session->set_command_host(this);
    session->set_system_host(this);
    session->set_lifecycle_host(this);

    // Send greeting.
    const char* greeting = "HPActor CLI — Type /help for commands, /quit to exit.\n";
    ssize_t ignored = ::write(client_fd, greeting, std::strlen(greeting));
    (void)ignored;
    const char sentinel = '\0';
    ignored = ::write(client_fd, &sentinel, 1);
    (void)ignored;

    SessionState state;
    state.session = std::move(session);
    state.last_activity = std::chrono::steady_clock::now();
    state.seqno = next_seqno_++;
    // Build "IP:port" string for TCP clients.
    if (auto* ipv4 = std::get_if<Ipv4Endpoint>(&remote_ep)) {
        char buf[32];
        uint32_t a = ipv4->addr;
        snprintf(buf, sizeof(buf), "%u.%u.%u.%u:%u", (a >> 24) & 0xFF,
                 (a >> 16) & 0xFF, (a >> 8) & 0xFF, a & 0xFF, ipv4->port());
        state.remote_addr = buf;
    } else {
        state.remote_addr = "tcp";
    }
    int fd = client_fd;
    sessions_.emplace(fd, std::move(state));

    // Register a read_handler so the EventLoop wakes us when data arrives.
    loop_->set_read_handler(
        fd, [this](int ready_fd) { on_client_readable(ready_fd); });
}

void CliLegacyServerActor::on_uds_accepted(int client_fd) {
    if (sessions_.size() >= config_.max_sessions) {
        ::close(client_fd);
        return;
    }

    auto formatter = OutputFormatter::create(config_.default_format);
    auto session = std::make_unique<CliSession>(
        &system_, command_tree_.get(), std::move(formatter),
        [client_fd](const std::string& text) {
            // Best-effort write — the fd is non-blocking and may fail.
            ssize_t ignored = ::write(client_fd, text.data(), text.size());
            (void)ignored;
            const char sentinel = '\0';
            ignored = ::write(client_fd, &sentinel, 1);
            (void)ignored;
        },
        config_.page_size);
    session->set_cli_server_actor(this);
    session->set_command_host(this);
    session->set_system_host(this);
    session->set_lifecycle_host(this);

    // Send greeting.
    const char* greeting = "HPActor CLI — Type /help for commands, /quit to exit.\n";
    ssize_t ignored = ::write(client_fd, greeting, std::strlen(greeting));
    (void)ignored;
    const char sentinel = '\0';
    ignored = ::write(client_fd, &sentinel, 1);
    (void)ignored;

    SessionState state;
    state.session = std::move(session);
    state.last_activity = std::chrono::steady_clock::now();
    state.seqno = next_seqno_++;
    state.remote_addr = "uds";
    int fd = client_fd;
    sessions_.emplace(fd, std::move(state));

    // Register a read_handler so the EventLoop wakes us when data arrives.
    loop_->set_read_handler(
        fd, [this](int ready_fd) { on_client_readable(ready_fd); });
}

void CliLegacyServerActor::on_client_readable(int client_fd) {
    auto it = sessions_.find(client_fd);
    if (it == sessions_.end())
        return;

    auto& state = it->second;
    state.last_activity = std::chrono::steady_clock::now();

    // Drain available data.
    char buf[4096];
    bool closed = false;
    while (!closed) {
        ssize_t n = ::read(client_fd, buf, sizeof(buf));
        if (n > 0) {
            state.read_buffer.append(buf, static_cast<size_t>(n));
            // Process all complete lines.
            size_t nl;
            while ((nl = state.read_buffer.find('\n')) != std::string::npos) {
                std::string line = state.read_buffer.substr(0, nl);
                state.read_buffer.erase(0, nl + 1);
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();

                if (!state.session->process_line(line)) {
                    close_session(client_fd);
                    return;
                }

                // Send end-of-response sentinel.
                const char nul = '\0';
                ssize_t ign = ::write(client_fd, &nul, 1);
                (void)ign;
            }
        } else if (n == 0) {
            closed = true;
        } else {
            // n < 0
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            closed = true;
        }
    }

    if (closed) {
        close_session(client_fd);
    }
}

void CliLegacyServerActor::close_session(int client_fd) {
    auto it = sessions_.find(client_fd);
    if (it == sessions_.end())
        return;

    loop_->clear_read_handler(client_fd);
    ::close(client_fd);
    sessions_.erase(it);
}

// ---------------------------------------------------------------------------
// Client management (for /client commands)
// ---------------------------------------------------------------------------

std::string CliLegacyServerActor::list_clients() const {
    std::string result;
    if (sessions_.empty())
        return "No connected clients.\n";
    result += "Seqno  Remote\n";
    result += "-----  ------\n";
    for (const auto& [fd, state] : sessions_) {
        char buf[80];
        snprintf(buf, sizeof(buf), "%-6u %s\n", state.seqno,
                 state.remote_addr.c_str());
        result += buf;
    }
    return result;
}

bool CliLegacyServerActor::close_client(uint32_t seqno) {
    for (auto& [fd, state] : sessions_) {
        if (state.seqno == seqno) {
            // Need to remove read handler before closing fd.
            // close_session is const-safe if we cast away the sessions_ lookup
            // — but it's a private method on `this`, so we can just call it
            // directly.  Copy the closure logic inline to avoid modifying
            // close_session to accept a seqno.
            loop_->clear_read_handler(fd);
            ::close(fd);
            sessions_.erase(fd);
            return true;
        }
    }
    return false;
}

std::string CliLegacyServerActor::client_history(uint32_t seqno) const {
    for (const auto& [fd, state] : sessions_) {
        if (state.seqno == seqno)
            return "(not tracked for text-based sessions)\n";
    }
    return "Client " + std::to_string(seqno) + " not found.\n";
}

// ---------------------------------------------------------------------------
// Metadata
// ---------------------------------------------------------------------------

cli::ActorMeta CliLegacyServerActor::to_metadata() const {
    return host_impl_.make_metadata(id(), type_name(), running_);
}

// ---------------------------------------------------------------------------
// Command tree
// ---------------------------------------------------------------------------

void CliLegacyServerActor::build_command_tree() {
    command_tree_ = host_impl_.build_command_tree();
}

// ---------------------------------------------------------------------------
// Request-Response Helpers (mirrors CliActor's methods)
// ---------------------------------------------------------------------------

std::optional<InspectStateReply>
CliLegacyServerActor::inspect(ActorId target, const InspectStateRequest& req,
                              std::chrono::milliseconds timeout) {
    if (target == id()) {
        InspectStateReply reply;
        auto* pb_meta = reply.mutable_metadata();
        pb_meta->set_actor_id(id().value());
        pb_meta->set_actor_type(std::string(type_name()));
        pb_meta->set_state("Running");
        return reply;
    }
    return host_impl_.inspect(target, req, mailbox(), address(), timeout);
}

std::optional<KillReply>
CliLegacyServerActor::kill(ActorId target, const KillRequest& req,
                           std::chrono::milliseconds timeout) {
    return host_impl_.kill(target, req, mailbox(), address(), timeout);
}

std::optional<QuarantineReply>
CliLegacyServerActor::quarantine(ActorId target, const QuarantineRequest& req,
                                 std::chrono::milliseconds timeout) {
    return host_impl_.quarantine(target, req, mailbox(), address(), timeout);
}

std::vector<ActorMeta> CliLegacyServerActor::enumerate(std::string_view filter) {
    return host_impl_.enumerate(filter);
}

// execute_path is inline in header (returns false — local host).

result<void> CliLegacyServerActor::dlq_replay(uint32_t index, ActorId target) {
    return host_impl_.dlq_replay(index, target, address());
}

// ---------------------------------------------------------------------------
// ILifecycleCliHost interface
// ---------------------------------------------------------------------------

result<void> CliLegacyServerActor::drain() {
    return host_impl_.drain();
}

result<void> CliLegacyServerActor::shutdown() {
    return host_impl_.shutdown();
}

} // namespace cli
} // namespace hpactor
