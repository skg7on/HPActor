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
#include <hpactor/cli_messages.pb.h>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/net/acceptor.hpp>
#include <hpactor/net/event_loop.hpp>
#include <hpactor/types/types.hpp>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <grp.h>
#include <pwd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <thread>
#include <unordered_map>

namespace hpactor {
namespace cli {

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

CliServerActor::CliServerActor(ActorContext* ctx, ActorSystem& system,
                               const CliServerConfig& config)
    : DaemonActor(ctx, system), system_(system), config_(config),
      loop_(std::make_unique<net::EventLoop>()) {}

CliServerActor::~CliServerActor() = default;

// ---------------------------------------------------------------------------
// DaemonActor interface
// ---------------------------------------------------------------------------

void CliServerActor::on_daemon_start() {
    build_command_tree();

    // --- Unix domain socket ---
    if (!config_.uds_listen_path.empty()) {
        // Remove stale socket file before binding.
        ::unlink(config_.uds_listen_path.c_str());

        uds_acceptor_ = std::make_unique<net::UnixDomainAcceptor>(loop_.get());
        uds_acceptor_->set_accept_handler(
            [this](int fd, EndPoint /*remote*/) { on_client_accepted(fd); });

        if (!uds_acceptor_->listen(config_.uds_listen_path)) {
            std::fprintf(stderr, "CliServerActor: UDS listen failed on %s\n",
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
        tcp_acceptor_->set_accept_handler(
            [this](int fd, EndPoint /*remote*/) { on_client_accepted(fd); });

        if (!tcp_acceptor_->listen(config_.tcp_listen_port, 0,
                                   config_.tcp_bind_address)) {
            std::fprintf(stderr, "CliServerActor: TCP listen failed on %s:%u\n",
                         config_.tcp_bind_address.c_str(),
                         static_cast<unsigned>(config_.tcp_listen_port));
            tcp_acceptor_.reset();
            // UDS may still be working — don't abort.
        }
    }

    if (!uds_acceptor_ && !tcp_acceptor_) {
        std::fprintf(stderr, "CliServerActor: no listeners configured\n");
        running_ = false;
    }
}

void CliServerActor::on_daemon_stop() {
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

bool CliServerActor::run_once() {
    if (!running_)
        return false;

    // Drive the EventLoop — same pattern as HTTPGateway::run_once().
    loop_->wait(100);

    return running_;
}

// ---------------------------------------------------------------------------
// Client connection management
// ---------------------------------------------------------------------------

void CliServerActor::on_client_accepted(int client_fd) {
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
    int fd = client_fd;
    sessions_.emplace(fd, std::move(state));

    // Register a read_handler so the EventLoop wakes us when data arrives.
    loop_->set_read_handler(
        fd, [this](int ready_fd) { on_client_readable(ready_fd); });
}

void CliServerActor::on_client_readable(int client_fd) {
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

void CliServerActor::close_session(int client_fd) {
    auto it = sessions_.find(client_fd);
    if (it == sessions_.end())
        return;

    loop_->clear_read_handler(client_fd);
    ::close(client_fd);
    sessions_.erase(it);
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

// ---------------------------------------------------------------------------
// Command tree
// ---------------------------------------------------------------------------

void CliServerActor::build_command_tree() {
    auto root = std::make_unique<CommandNode>("/", "CLI root");
    build_command_tree_from_registry(*root);
    command_tree_ = std::move(root);
}

// ---------------------------------------------------------------------------
// Request-Response Helpers (mirrors CliActor's methods)
// ---------------------------------------------------------------------------

namespace {

std::optional<StreamBuffer>
poll_for_response(mailbox::MPSCActorMailbox<TypedMessage>* mbox,
                  TypeTag expected_tag, std::chrono::milliseconds timeout) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        TypedMessage msg;
        if (mbox->try_pop(msg)) {
            if (msg.type_id() == expected_tag)
                return std::move(msg).payload();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return std::nullopt;
}

} // anonymous namespace

std::optional<InspectStateReply>
CliServerActor::send_and_wait_inspect(ActorId target, const InspectStateRequest& req,
                                      std::chrono::milliseconds timeout) {
    if (target == id()) {
        InspectStateReply reply;
        auto* pb_meta = reply.mutable_metadata();
        pb_meta->set_actor_id(id().value());
        pb_meta->set_actor_type(std::string(type_name()));
        pb_meta->set_state("Running");
        return reply;
    }
    auto actor = system_.get_actor(target);
    if (!actor)
        return std::nullopt;

    auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        TypedMessage msg(TypeTag::InspectStateRequestTag, req);
        msg.set_sender_address(address());
        auto enqueue_result = system_.try_deliver_local(target, std::move(msg));
        if (enqueue_result.accepted())
            break;
        if (std::chrono::steady_clock::now() >= deadline)
            return std::nullopt;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    auto payload =
        poll_for_response(mailbox(), TypeTag::InspectStateResponseTag, timeout);
    if (!payload)
        return std::nullopt;
    InspectStateReply reply;
    if (!reply.ParseFromArray(payload->data(), static_cast<int>(payload->size())))
        return std::nullopt;
    std::string wire = reply.SerializeAsString();
    InspectStateReply safe_reply;
    if (!safe_reply.ParseFromString(wire))
        return std::nullopt;
    return safe_reply;
}

std::optional<KillReply>
CliServerActor::send_and_wait_kill(ActorId target, const KillRequest& req,
                                   std::chrono::milliseconds timeout) {
    auto actor = system_.get_actor(target);
    if (!actor)
        return std::nullopt;
    auto kill_deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        TypedMessage msg(TypeTag::KillRequestTag, req);
        msg.set_sender_address(address());
        auto enqueue_result = system_.try_deliver_local(target, std::move(msg));
        if (enqueue_result.accepted())
            break;
        if (std::chrono::steady_clock::now() >= kill_deadline)
            return std::nullopt;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    auto payload = poll_for_response(mailbox(), TypeTag::KillResponseTag, timeout);
    if (!payload)
        return std::nullopt;
    KillReply reply;
    if (!reply.ParseFromArray(payload->data(), static_cast<int>(payload->size())))
        return std::nullopt;
    std::string wire = reply.SerializeAsString();
    KillReply safe_reply;
    if (!safe_reply.ParseFromString(wire))
        return std::nullopt;
    return safe_reply;
}

std::optional<QuarantineReply>
CliServerActor::send_and_wait_quarantine(ActorId target,
                                         const QuarantineRequest& req,
                                         std::chrono::milliseconds timeout) {
    auto actor = system_.get_actor(target);
    if (!actor)
        return std::nullopt;
    auto q_deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        TypedMessage msg(TypeTag::QuarantineRequestTag, req);
        msg.set_sender_address(address());
        auto enqueue_result = system_.try_deliver_local(target, std::move(msg));
        if (enqueue_result.accepted())
            break;
        if (std::chrono::steady_clock::now() >= q_deadline)
            return std::nullopt;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    auto payload =
        poll_for_response(mailbox(), TypeTag::QuarantineResponseTag, timeout);
    if (!payload)
        return std::nullopt;
    QuarantineReply reply;
    if (!reply.ParseFromArray(payload->data(), static_cast<int>(payload->size())))
        return std::nullopt;
    std::string wire = reply.SerializeAsString();
    QuarantineReply safe_reply;
    if (!safe_reply.ParseFromString(wire))
        return std::nullopt;
    return safe_reply;
}

std::vector<ActorMeta> CliServerActor::enumerate_actors(const std::string& filter) {
    std::vector<ActorMeta> result;
    system_.for_each_actor([&](ActorId /*id*/, AbstractActor& actor) {
        if (!filter.empty()) {
            std::string tn(actor.type_name().data(), actor.type_name().size());
            if (tn.find(filter) == std::string::npos)
                return;
        }
        result.push_back(actor.to_metadata());
    });
    return result;
}

} // namespace cli
} // namespace hpactor
