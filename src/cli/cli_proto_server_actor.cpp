// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#include <hpactor/cli.pb.h>
#include <hpactor/cli/cli_proto_server_actor.hpp>
#include <hpactor/cli/cli_session.hpp>
#include <hpactor/cli/command_context.hpp>
#include <hpactor/cli/command_node.hpp>
#include <hpactor/cli/command_registry.hpp>
#include <hpactor/cli/command_tree_builder.hpp>
#include <hpactor/cli/output_formatter.hpp>
#include <hpactor/cli_messages.pb.h>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/fault/fault_controller.hpp>
#include <hpactor/fault/fault_macros.hpp>
#include <hpactor/mailbox/dead_letter_queue.hpp>
#include <hpactor/mem/memory_region.hpp>
#include <hpactor/metrics/metrics_actor.hpp>
#include <hpactor/msg/dead_letter_record.hpp>
#include <hpactor/msg/frame.hpp>
#include <hpactor/net/acceptor.hpp>
#include <hpactor/net/event_loop.hpp>
#include <hpactor/net/wireframe_connection.hpp>
#include <hpactor/types/types.hpp>

#include "commands/command_utils.hpp"

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

namespace {

/// Get the local endpoint of a socket fd via getsockname.
/// For TCP/IPv4 sockets. Returns LocalEndpoint on failure.
EndPoint get_local_endpoint(int fd) {
    struct sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    if (::getsockname(fd, reinterpret_cast<struct sockaddr*>(&addr), &len) == 0) {
        return Ipv4Endpoint{addr.sin_addr.s_addr, addr.sin_port};
    }
    return LocalEndpoint;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

CliProtoServerActor::CliProtoServerActor(ActorContext* ctx, ActorSystem& system,
                                         const CliProtoServerConfig& config)
    : DaemonActor(ctx, system), system_(system), config_(config),
      loop_(std::make_unique<net::EventLoop>()), host_impl_(system) {}

CliProtoServerActor::~CliProtoServerActor() = default;

// ---------------------------------------------------------------------------
// DaemonActor interface
// ---------------------------------------------------------------------------

void CliProtoServerActor::on_daemon_start() {
    build_command_tree();

    // Register a send-completion callback so WireFrameConnection writes
    // through this EventLoop correctly clear is_sending_ after each send.
    // Without this, handle_send_completion is never called and subsequent
    // sends silently queue without flushing → client timeouts.
    loop_->set_completion_callback([this](net::OpCompletion c) {
        if (c.type == net::OpType::Send) {
            auto it = sessions_.find(c.fd);
            if (it != sessions_.end() && it->second.conn) {
                it->second.conn->handle_send_completion(c.result);
            }
        }
    });

    // --- Unix domain socket ---
    if (!config_.uds_listen_path.empty()) {
        ::unlink(config_.uds_listen_path.c_str());

        uds_acceptor_ = std::make_unique<net::UnixDomainAcceptor>(loop_.get());
        uds_acceptor_->set_accept_handler(
            [this](int fd, EndPoint /*remote*/) { on_uds_accepted(fd); });

        if (!uds_acceptor_->listen(config_.uds_listen_path)) {
            std::fprintf(stderr, "CliProtoServerActor: UDS listen failed on %s\n",
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
            std::fprintf(stderr, "CliProtoServerActor: TCP listen failed on %s:%u\n",
                         config_.tcp_bind_address.c_str(),
                         static_cast<unsigned>(config_.tcp_listen_port));
            tcp_acceptor_.reset();
        }
    }

    if (!uds_acceptor_ && !tcp_acceptor_) {
        std::fprintf(stderr, "CliProtoServerActor: no listeners configured\n");
        running_ = false;
    }
}

void CliProtoServerActor::on_daemon_stop() {
    running_ = false;

    // Close all proto sessions.
    for (auto& [fd, state] : sessions_) {
        if (state.conn) {
            state.conn->close();
        }
    }
    sessions_.clear();

    // Acceptors self-close on destruction.
    uds_acceptor_.reset();
    tcp_acceptor_.reset();

    if (!config_.uds_listen_path.empty())
        ::unlink(config_.uds_listen_path.c_str());

    command_tree_.reset();
}

bool CliProtoServerActor::run_once() {
    if (!running_)
        return false;

    // Check idle timeout on sessions.
    auto now = std::chrono::steady_clock::now();
    std::vector<int> to_close;
    for (auto& [fd, state] : sessions_) {
        if (now - state.last_activity > config_.session_timeout) {
            to_close.push_back(fd);
        }
    }
    for (int fd : to_close) {
        close_proto_session(fd);
    }

    // Process any pending send completions before blocking in wait().
    loop_->process_completions();
    loop_->wait(100);

    return running_;
}

// ---------------------------------------------------------------------------
// Client connection management
// ---------------------------------------------------------------------------

void CliProtoServerActor::on_tcp_accepted(int client_fd, EndPoint remote_ep) {
    if (sessions_.size() >= config_.max_sessions) {
        ::close(client_fd);
        return;
    }

    EndPoint local_ep = get_local_endpoint(client_fd);

    auto conn = net::WireFrameConnection::create_as_server(
        client_fd, local_ep, remote_ep, loop_.get());

    if (!conn) {
        ::close(client_fd);
        return;
    }

    auto formatter = OutputFormatter::create(config_.default_format);
    auto session = std::make_unique<CliSession>(
        &system_, command_tree_.get(), std::move(formatter),
        [this, client_fd](const std::string& text) {
            // Build a CliResponse from the output text.
            CliResponse resp;
            resp.set_content_type("text/plain");
            resp.set_payload(text);
            send_hpac_frame(client_fd, resp.SerializeAsString());
        },
        config_.page_size);
    session->set_command_host(this);
    session->set_system_host(this);
    session->set_lifecycle_host(this);
    session->set_proto_server(this);

    SessionState state;
    state.conn = conn;
    state.session = std::move(session);
    state.last_activity = std::chrono::steady_clock::now();
    state.seqno = next_seqno_++;
    // Build "IP:port" string for TCP clients.
    // Ipv4Endpoint stores addr/port in network byte order (big-endian).
    if (auto* ipv4 = std::get_if<Ipv4Endpoint>(&remote_ep)) {
        char buf[32];
        uint32_t a = ipv4->addr;
        snprintf(buf, sizeof(buf), "%u.%u.%u.%u:%u", (a >> 24) & 0xFF,
                 (a >> 16) & 0xFF, (a >> 8) & 0xFF, a & 0xFF, ipv4->port());
        state.remote_addr = buf;
    } else {
        state.remote_addr = "tcp";
    }
    sessions_.emplace(client_fd, std::move(state));

    // Set up frame handler — WireFrameConnection auto-decodes HPAC frames.
    conn->set_frame_handler([this, client_fd](adt::StreamBuffer data) {
        on_frame_received(client_fd, std::move(data));
    });

    // Set up error handler for disconnect cleanup.
    std::weak_ptr<net::WireFrameConnection> weak_conn = conn;
    conn->set_error_handler(
        [this, client_fd](net::ConnectionPtr /*conn*/, const error& /*err*/) {
            close_proto_session(client_fd);
        });
}

void CliProtoServerActor::on_uds_accepted(int client_fd) {
    if (sessions_.size() >= config_.max_sessions) {
        ::close(client_fd);
        return;
    }

    auto conn = net::WireFrameConnection::create_as_server(
        client_fd, LocalEndpoint, LocalEndpoint, loop_.get());

    if (!conn) {
        ::close(client_fd);
        return;
    }

    auto formatter = OutputFormatter::create(config_.default_format);
    auto session = std::make_unique<CliSession>(
        &system_, command_tree_.get(), std::move(formatter),
        [this, client_fd](const std::string& text) {
            CliResponse resp;
            resp.set_content_type("text/plain");
            resp.set_payload(text);
            send_hpac_frame(client_fd, resp.SerializeAsString());
        },
        config_.page_size);
    session->set_command_host(this);
    session->set_system_host(this);
    session->set_lifecycle_host(this);
    session->set_proto_server(this);

    SessionState state;
    state.conn = conn;
    state.session = std::move(session);
    state.last_activity = std::chrono::steady_clock::now();
    state.seqno = next_seqno_++;
    state.remote_addr = "uds";
    sessions_.emplace(client_fd, std::move(state));

    conn->set_frame_handler([this, client_fd](adt::StreamBuffer data) {
        on_frame_received(client_fd, std::move(data));
    });

    conn->set_error_handler(
        [this, client_fd](net::ConnectionPtr /*conn*/, const error& /*err*/) {
            close_proto_session(client_fd);
        });
}

void CliProtoServerActor::on_frame_received(int client_fd, adt::StreamBuffer data) {
    auto it = sessions_.find(client_fd);
    if (it == sessions_.end())
        return;

    auto& state = it->second;
    state.last_activity = std::chrono::steady_clock::now();

    // The WireFrameConnection delivers the full HPAC frame body (magic +
    // length already stripped).  The body is the serialized CliCommand.
    CliCommand cmd;
    if (!cmd.ParseFromArray(data.data(), static_cast<int>(data.size()))) {
        if (state.command_history.size() < 10000)
            state.command_history.emplace_back("(invalid)");
        CliResponse resp;
        resp.set_content_type("application/x-protobuf");
        resp.set_is_error(true);
        resp.set_error_code(1);
        send_hpac_frame(client_fd, resp.SerializeAsString());
        return;
    }

    // Record command in session history.
    if (state.command_history.size() < 10000) {
        if (!cmd.rpc_method().empty())
            state.command_history.emplace_back(cmd.rpc_method());
        else if (!cmd.path().empty())
            state.command_history.emplace_back(cmd.path());
    }

    // Dual dispatch: rpc_method takes priority over path.
    if (!cmd.rpc_method().empty()) {
        std::string rpc_result = dispatch_rpc(cmd.rpc_method(), cmd.rpc_request());
        CliResponse resp;
        resp.set_content_type("application/x-protobuf");
        resp.set_payload(rpc_result);
        resp.set_is_structured(true);
        if (rpc_result.empty()) {
            resp.set_is_error(true);
            resp.set_error_code(2); // method not found
        }
        send_hpac_frame(client_fd, resp.SerializeAsString());
        return;
    }

    // Command-tree dispatch.
    if (cmd.path().empty()) {
        CliResponse resp;
        resp.set_content_type("text/plain");
        resp.set_payload("Error: no path or rpc_method specified\n");
        resp.set_is_error(true);
        resp.set_error_code(1);
        send_hpac_frame(client_fd, resp.SerializeAsString());
        return;
    }

    // Build a command-line string from the path + params + args.
    // Convert slash-separated path (e.g., "system/memory") to
    // space-separated command-line tokens ("system memory") expected
    // by CliSession::process_line / execute_tokens.
    std::string command_line = cmd.path();
    for (char& c : command_line) {
        if (c == '/')
            c = ' ';
    }
    for (const auto& [key, value] : cmd.params()) {
        command_line += " --" + key;
        if (!value.empty()) {
            command_line += " " + value;
        }
    }
    for (const auto& arg : cmd.args()) {
        command_line += " " + arg;
    }

    // Create a temporary session for command-tree dispatch.
    std::string output_buffer;
    auto temp_formatter = OutputFormatter::create(
        cmd.format().empty() ? config_.default_format : cmd.format());
    std::string* buf_ptr = &output_buffer;
    auto output_fn = [buf_ptr](const std::string& text) { *buf_ptr += text; };

    auto temp_session = std::make_unique<CliSession>(
        &system_, command_tree_.get(), std::move(temp_formatter), output_fn,
        config_.page_size);
    temp_session->set_command_host(this);
    temp_session->set_system_host(this);
    temp_session->set_lifecycle_host(this);

    temp_session->process_line(command_line);

    std::string format =
        cmd.format().empty() ? config_.default_format : cmd.format();
    CliResponse resp;
    if (format == "json") {
        resp.set_content_type("application/json");
    } else {
        resp.set_content_type("text/plain");
    }
    resp.set_payload(output_buffer);
    resp.set_is_structured(false);

    send_hpac_frame(client_fd, resp.SerializeAsString());
}

void CliProtoServerActor::send_hpac_frame(int client_fd, const std::string& data) {
    auto it = sessions_.find(client_fd);
    if (it == sessions_.end())
        return;

    auto frame_data = encode_as_frame(data);
    it->second.conn->send(frame_data);
}

void CliProtoServerActor::close_proto_session(int client_fd) {
    auto it = sessions_.find(client_fd);
    if (it == sessions_.end())
        return;

    if (it->second.conn) {
        it->second.conn->close();
    }
    sessions_.erase(it);
}

// ---------------------------------------------------------------------------
// Client management (for /client commands)
// ---------------------------------------------------------------------------

std::string CliProtoServerActor::list_clients() const {
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

bool CliProtoServerActor::close_client(uint32_t seqno) {
    for (auto& [fd, state] : sessions_) {
        if (state.seqno == seqno) {
            close_proto_session(fd);
            return true;
        }
    }
    return false;
}

std::string CliProtoServerActor::client_history(uint32_t seqno) const {
    for (const auto& [fd, state] : sessions_) {
        if (state.seqno == seqno) {
            if (state.command_history.empty())
                return "(no commands recorded)\n";
            std::string result =
                "Command history for client " + std::to_string(seqno) + " (" +
                std::to_string(state.command_history.size()) + " entries):\n";
            for (size_t i = 0; i < state.command_history.size(); ++i) {
                result += std::to_string(i + 1) + ". " +
                          state.command_history[i] + "\n";
            }
            return result;
        }
    }
    return "Client " + std::to_string(seqno) + " not found.\n";
}

// ---------------------------------------------------------------------------
// Metadata
// ---------------------------------------------------------------------------

cli::ActorMeta CliProtoServerActor::to_metadata() const {
    return host_impl_.make_metadata(id(), type_name(), running_);
}

// ---------------------------------------------------------------------------
// Command tree
// ---------------------------------------------------------------------------

void CliProtoServerActor::build_command_tree() {
    command_tree_ = host_impl_.build_command_tree();
}

// ---------------------------------------------------------------------------
// ICliCommandHost interface (copied from CliLegacyServerActor)
// ---------------------------------------------------------------------------

std::optional<InspectStateReply>
CliProtoServerActor::inspect(ActorId target, const InspectStateRequest& req,
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
CliProtoServerActor::kill(ActorId target, const KillRequest& req,
                          std::chrono::milliseconds timeout) {
    return host_impl_.kill(target, req, mailbox(), address(), timeout);
}

std::optional<QuarantineReply>
CliProtoServerActor::quarantine(ActorId target, const QuarantineRequest& req,
                                std::chrono::milliseconds timeout) {
    return host_impl_.quarantine(target, req, mailbox(), address(), timeout);
}

std::vector<ActorMeta> CliProtoServerActor::enumerate(std::string_view filter) {
    return host_impl_.enumerate(filter);
}

// execute_path is inline in header (returns false — local host).

result<void> CliProtoServerActor::dlq_replay(uint32_t index, ActorId target) {
    return host_impl_.dlq_replay(index, target, address());
}

// ---------------------------------------------------------------------------
// ILifecycleCliHost interface
// ---------------------------------------------------------------------------

result<void> CliProtoServerActor::drain() {
    return host_impl_.drain();
}

result<void> CliProtoServerActor::shutdown() {
    return host_impl_.shutdown();
}

// ---------------------------------------------------------------------------
// Structured RPC dispatch (copied from CliLegacyServerActor)
// ---------------------------------------------------------------------------

std::string CliProtoServerActor::dispatch_rpc(const std::string& rpc_method,
                                              const std::string& rpc_request) {
    if (rpc_method == "inspect") {
        InspectStateRequest req;
        if (!req.ParseFromString(rpc_request)) {
            return "";
        }
        auto reply = inspect(ActorId(req.target_actor_id()), req);
        if (!reply)
            return "";
        return reply->SerializeAsString();
    }

    if (rpc_method == "kill") {
        KillRequest req;
        if (!req.ParseFromString(rpc_request)) {
            return "";
        }
        auto reply = kill(ActorId(req.target_actor_id()), req);
        if (!reply)
            return "";
        return reply->SerializeAsString();
    }

    if (rpc_method == "quarantine") {
        QuarantineRequest req;
        if (!req.ParseFromString(rpc_request)) {
            return "";
        }
        auto reply = quarantine(ActorId(req.target_actor_id()), req);
        if (!reply)
            return "";
        return reply->SerializeAsString();
    }

    if (rpc_method == "enumerate") {
        std::string_view filter;
        ListActorsRequest list_req;
        if (list_req.ParseFromString(rpc_request)) {
            filter = list_req.filter();
        }
        auto actors = enumerate(filter);
        ListActorsReply reply;
        for (auto& a : actors) {
            auto* pb_meta = reply.add_actors();
            pb_meta->set_actor_id(a.actor_id);
            pb_meta->set_actor_type(a.actor_type);
            pb_meta->set_state(a.state);
            pb_meta->set_incarnation(a.incarnation);
            pb_meta->set_messages_processed(a.messages_processed);
            pb_meta->set_uptime_ms(a.uptime_ms);
            if (!a.behavior_name.empty())
                pb_meta->set_behavior_name(a.behavior_name);
        }
        reply.set_total_count(static_cast<uint32_t>(actors.size()));
        return reply.SerializeAsString();
    }

    if (rpc_method == "system_stats") {
        SystemStatsReply reply;
        reply.set_total_actors(system_.actor_count());
        if (auto* sched = system_.scheduler()) {
            reply.set_worker_count(static_cast<uint32_t>(sched->worker_count()));
        }
        return reply.SerializeAsString();
    }

    if (rpc_method == "memory_stats") {
        MemoryStatsReply reply;
        auto& reg = mem::MemoryRegionRegistry::instance();
        auto snap = reg.snapshot(mem::RegionType::kActor);
        reply.set_active_bytes(snap.active_bytes);
        reply.set_peak_bytes(snap.limit.hard_limit_bytes);
        reply.set_segment_count(static_cast<uint32_t>(snap.alloc_count));
        reply.set_slab_hit_rate(0.0);
        return reply.SerializeAsString();
    }

    // Unknown method.
    return "";
}

} // namespace cli
} // namespace hpactor
