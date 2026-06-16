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
#include <hpactor/msg/dead_letter_record.hpp>
#include <hpactor/msg/frame.hpp>
#include <hpactor/net/acceptor.hpp>
#include <hpactor/net/event_loop.hpp>
#include <hpactor/net/wireframe_connection.hpp>
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

/// Poll the mailbox for a response with a specific TypeTag.
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

/// Encode protobuf data as an HPAC WireFrame: magic + big-endian length + data.
StreamBuffer encode_as_frame(const std::string& protobuf_data) {
    StreamBuffer result;
    const std::array<uint8_t, 4> magic = {'H', 'P', 'A', 'C'};
    result.append(magic.data(), 4);
    uint32_t payload_len = static_cast<uint32_t>(protobuf_data.size());
    uint32_t net_len = htonl(payload_len);
    result.append(reinterpret_cast<const uint8_t*>(&net_len), 4);
    result.append(reinterpret_cast<const uint8_t*>(protobuf_data.data()),
                  protobuf_data.size());
    return result;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

CliProtoServerActor::CliProtoServerActor(ActorContext* ctx, ActorSystem& system,
                                         const CliProtoServerConfig& config)
    : DaemonActor(ctx, system), system_(system), config_(config),
      loop_(std::make_unique<net::EventLoop>()) {}

CliProtoServerActor::~CliProtoServerActor() = default;

// ---------------------------------------------------------------------------
// DaemonActor interface
// ---------------------------------------------------------------------------

void CliProtoServerActor::on_daemon_start() {
    build_command_tree();

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

    SessionState state;
    state.conn = conn;
    state.session = std::move(session);
    state.last_activity = std::chrono::steady_clock::now();
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

    SessionState state;
    state.conn = conn;
    state.session = std::move(session);
    state.last_activity = std::chrono::steady_clock::now();
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
        // Invalid protobuf — send error response.
        CliResponse resp;
        resp.set_content_type("application/x-protobuf");
        resp.set_is_error(true);
        resp.set_error_code(1);
        send_hpac_frame(client_fd, resp.SerializeAsString());
        return;
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
    std::string command_line = cmd.path();
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
// Metadata
// ---------------------------------------------------------------------------

cli::ActorMeta CliProtoServerActor::to_metadata() const {
    cli::ActorMeta m;
    m.actor_id = id().value();
    m.actor_type = std::string(type_name());
    m.state = running_ ? "Running" : "Stopped";
    return m;
}

// ---------------------------------------------------------------------------
// Command tree
// ---------------------------------------------------------------------------

void CliProtoServerActor::build_command_tree() {
    auto root = std::make_unique<CommandNode>("/", "CLI root");
    build_command_tree_from_registry(*root);
    command_tree_ = std::move(root);
}

// ---------------------------------------------------------------------------
// ICliCommandHost interface (copied from CliServerActor)
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
CliProtoServerActor::kill(ActorId target, const KillRequest& req,
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
CliProtoServerActor::quarantine(ActorId target, const QuarantineRequest& req,
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

std::vector<ActorMeta> CliProtoServerActor::enumerate(std::string_view filter) {
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

// ---------------------------------------------------------------------------
// ISystemCliHost interface (copied from CliServerActor)
// ---------------------------------------------------------------------------

void CliProtoServerActor::render_system_stats(OutputFormatter& output) {
    output.header("System Statistics");
    std::map<std::string, std::string> kv;
    kv["Total actors"] = std::to_string(system_.actor_count());
    if (auto* sched = system_.scheduler()) {
        kv["Scheduler threads"] = std::to_string(sched->worker_count());
    }
    output.key_value(kv);
}

void CliProtoServerActor::render_memory_stats(OutputFormatter& output) {
    output.header("Memory Regions");
    auto& reg = mem::MemoryRegionRegistry::instance();
    std::vector<std::string> cols = {"Region",     "Active", "Limit",
                                     "Pressure",   "Allocs", "Frees",
                                     "Corruptions"};
    std::vector<std::vector<std::string>> rows;
    static constexpr mem::RegionType kRegions[] = {
        mem::RegionType::kActor,     mem::RegionType::kMessage,
        mem::RegionType::kCoroutine, mem::RegionType::kNetwork,
        mem::RegionType::kInternal,  mem::RegionType::kHibernate};
    for (auto region : kRegions) {
        auto snap = reg.snapshot(region);
        rows.push_back({
            mem::to_string(region),
            std::to_string(snap.active_bytes),
            std::to_string(snap.limit.hard_limit_bytes),
            mem::to_string(snap.pressure),
            std::to_string(snap.alloc_count),
            std::to_string(snap.free_count),
            std::to_string(snap.corruption_events),
        });
    }
    output.table(cols, rows);
}

void CliProtoServerActor::render_fault_status(OutputFormatter& output) {
    output.header("Fault Injection Status");
    auto& fc = system_.fault_controller();
    if (!fc.is_enabled()) {
        output.raw("Fault injection is disabled.\n");
        return;
    }
    std::map<std::string, std::string> kv;
    kv["Enabled"] = "yes";
    kv["Seed"] = std::to_string(fc.replay_seed());
    kv["Hooks triggered"] = std::to_string(fc.faults_fired());
    output.key_value(kv);
}

void CliProtoServerActor::render_dlq_list(OutputFormatter& output,
                                          std::string_view filter) {
    output.header("Dead Letter Queue");
    auto* dlq = system_.dead_letter_queue();
    if (!dlq) {
        output.raw("DLQ is not configured.\n");
        return;
    }
    auto records = dlq->snapshot_records();
    if (records.empty()) {
        output.raw("DLQ is empty.\n");
        return;
    }
    std::vector<std::string> cols = {"#", "Actor", "Reason", "Source", "Age"};
    std::vector<std::vector<std::string>> rows;
    for (size_t i = 0; i < records.size(); ++i) {
        auto& r = records[i];
        if (!filter.empty()) {
            std::string aid = std::to_string(r.target.id.value());
            if (aid.find(filter) == std::string::npos)
                continue;
        }
        rows.push_back({
            std::to_string(i),
            std::to_string(r.target.id.value()),
            mailbox::to_string(r.reason),
            mailbox::to_string(r.source),
            std::to_string(r.timestamp_ns / 1'000'000) + "ms",
        });
    }
    output.table(cols, rows);
}

result<void> CliProtoServerActor::dlq_replay(uint32_t index, ActorId target) {
    auto* dlq = system_.dead_letter_queue();
    if (!dlq)
        return result<void>::make(
            error(errors::actor_not_found, "DLQ not configured"));

    mailbox::DeadLetterRecord record;
    if (!dlq->try_pop_at(index, record))
        return result<void>::make(
            error(errors::invalid_argument, "DLQ index out of range"));

    TypedMessage msg(record.type_tag, std::move(record.payload_sample));
    msg.set_sender_address(address());
    auto enqueue_result = system_.try_deliver_local(target, std::move(msg));
    if (!enqueue_result.accepted())
        return result<void>::make(
            error(errors::mailbox_full, "replay delivery failed"));

    return result<void>::make();
}

// ---------------------------------------------------------------------------
// ILifecycleCliHost interface (copied from CliServerActor)
// ---------------------------------------------------------------------------

result<void> CliProtoServerActor::drain() {
    return system_.shutdown();
}

result<void> CliProtoServerActor::shutdown() {
    return system_.shutdown();
}

// ---------------------------------------------------------------------------
// Structured RPC dispatch (copied from CliServerActor)
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
        auto fmt = OutputFormatter::create(config_.default_format);
        render_system_stats(*fmt);
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
