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

#include <hpactor/cli.pb.h>
#include <hpactor/cli/cli_server_actor.hpp>
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
CliServerActor::inspect(ActorId target, const InspectStateRequest& req,
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
CliServerActor::kill(ActorId target, const KillRequest& req,
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
CliServerActor::quarantine(ActorId target, const QuarantineRequest& req,
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

std::vector<ActorMeta> CliServerActor::enumerate(std::string_view filter) {
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
// ISystemCliHost interface
// ---------------------------------------------------------------------------

void CliServerActor::render_system_stats(OutputFormatter& output) {
    output.header("System Statistics");
    std::map<std::string, std::string> kv;
    kv["Total actors"] = std::to_string(system_.actor_count());
    if (auto* sched = system_.scheduler()) {
        kv["Scheduler threads"] = std::to_string(sched->worker_count());
    }
    output.key_value(kv);
}

void CliServerActor::render_memory_stats(OutputFormatter& output) {
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

void CliServerActor::render_fault_status(OutputFormatter& output) {
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

void CliServerActor::render_dlq_list(OutputFormatter& output,
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

result<void> CliServerActor::dlq_replay(uint32_t index, ActorId target) {
    auto* dlq = system_.dead_letter_queue();
    if (!dlq)
        return result<void>::make(
            error(errors::actor_not_found, "DLQ not configured"));

    mailbox::DeadLetterRecord record;
    if (!dlq->try_pop_at(index, record))
        return result<void>::make(
            error(errors::invalid_argument, "DLQ index out of range"));

    // Reconstruct and re-deliver the message from the dead-letter record.
    TypedMessage msg(record.type_tag, std::move(record.payload_sample));
    msg.set_sender_address(address());
    auto enqueue_result = system_.try_deliver_local(target, std::move(msg));
    if (!enqueue_result.accepted())
        return result<void>::make(
            error(errors::mailbox_full, "replay delivery failed"));

    return result<void>::make();
}

// ---------------------------------------------------------------------------
// ILifecycleCliHost interface
// ---------------------------------------------------------------------------

result<void> CliServerActor::drain() {
    return system_.shutdown();
}

result<void> CliServerActor::shutdown() {
    return system_.shutdown();
}

// ---------------------------------------------------------------------------
// Proto client connection management
// ---------------------------------------------------------------------------

void CliServerActor::on_proto_client_accepted(int client_fd) {
    if (proto_sessions_.size() >= config_.max_sessions) {
        ::close(client_fd);
        return;
    }

    ProtoSessionState state;
    state.last_activity = std::chrono::steady_clock::now();
    int fd = client_fd;
    proto_sessions_.emplace(fd, std::move(state));

    loop_->set_read_handler(
        fd, [this](int ready_fd) { on_proto_client_readable(ready_fd); });
}

void CliServerActor::on_proto_client_readable(int client_fd) {
    auto it = proto_sessions_.find(client_fd);
    if (it == proto_sessions_.end())
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
            // Protocol detection: peek at the first 4 bytes to
            // determine if this is HTTP (starts with "GET ", "POST",
            // "PUT ", etc.) or varint-length-prefixed protobuf.
            while (true) {
                if (state.read_buffer.size() < 4)
                    break;

                // Check for HTTP method prefix.
                std::string_view peek(state.read_buffer.data(),
                                      state.read_buffer.size());
                bool is_http = false;
                if (peek.size() >= 4) {
                    std::string_view first4 = peek.substr(0, 4);
                    if (first4 == "GET " || first4 == "POST" ||
                        first4 == "PUT " || first4 == "HEAD" || first4 == "DELE" ||
                        first4 == "PATC" || first4 == "OPTI") {
                        is_http = true;
                    }
                }

                if (is_http) {
                    // HTTP request — look for the double CRLF that
                    // ends the headers, then look for Content-Length
                    // to read the body.
                    size_t header_end = state.read_buffer.find("\r\n\r\n");
                    if (header_end == std::string::npos) {
                        // Headers not yet complete; wait for more data.
                        break;
                    }

                    // Find Content-Length header.
                    size_t cl_pos = state.read_buffer.find("Content-Length:");
                    size_t body_start = header_end + 4;
                    size_t body_size = 0;
                    if (cl_pos != std::string::npos && cl_pos < header_end) {
                        size_t val_start =
                            state.read_buffer.find_first_not_of(" \t", cl_pos + 15);
                        size_t val_end = state.read_buffer.find('\r', val_start);
                        if (val_start != std::string::npos &&
                            val_end != std::string::npos) {
                            std::string cl_str(state.read_buffer.data() + val_start,
                                               val_end - val_start);
                            body_size = std::stoull(cl_str);
                        }
                    }

                    if (state.read_buffer.size() < body_start + body_size) {
                        // Body not yet complete; wait for more data.
                        break;
                    }

                    // Parse the body as a CliCommand.
                    std::string body(state.read_buffer.data() + body_start,
                                     body_size);
                    CliCommand cmd;
                    if (!cmd.ParseFromString(body)) {
                        // Invalid protobuf — send error response and close.
                        CliResponse resp;
                        resp.set_content_type("application/x-protobuf");
                        resp.set_is_error(true);
                        resp.set_error_code(1);
                        std::string wire = resp.SerializeAsString();
                        // Varint-length-prefix + payload
                        uint32_t wire_len = static_cast<uint32_t>(wire.size());
                        char len_buf[5];
                        size_t len_bytes = 0;
                        while (wire_len > 0x7f) {
                            len_buf[len_bytes++] =
                                static_cast<char>((wire_len & 0x7f) | 0x80);
                            wire_len >>= 7;
                        }
                        len_buf[len_bytes++] = static_cast<char>(wire_len & 0x7f);
                        ::write(client_fd, len_buf, len_bytes);
                        ::write(client_fd, wire.data(), wire.size());
                        close_proto_session(client_fd);
                        return;
                    }

                    CliResponse resp = execute_cli_command(cmd);
                    std::string wire = resp.SerializeAsString();
                    // Build HTTP response.
                    std::string http_resp = "HTTP/1.1 200 OK\r\n";
                    http_resp += "Content-Type: application/x-protobuf\r\n";
                    http_resp +=
                        "Content-Length: " + std::to_string(wire.size()) + "\r\n";
                    http_resp += "Connection: close\r\n";
                    http_resp += "\r\n";
                    http_resp += wire;
                    ::write(client_fd, http_resp.data(), http_resp.size());
                    close_proto_session(client_fd);
                    return;
                } else {
                    // HPAC Frame: 4-byte magic + 4-byte big-endian length +
                    // body.
                    static constexpr size_t kHpacHeaderSize = 8;
                    const char kHpacMagic[4] = {'H', 'P', 'A', 'C'};

                    if (state.read_buffer.size() < kHpacHeaderSize)
                        break;

                    if (std::memcmp(state.read_buffer.data(), kHpacMagic, 4) != 0) {
                        close_proto_session(client_fd);
                        return;
                    }

                    uint32_t msg_len;
                    std::memcpy(&msg_len, state.read_buffer.data() + 4, 4);
                    msg_len = ntohl(msg_len);

                    if (state.read_buffer.size() < kHpacHeaderSize + msg_len)
                        break;

                    // Parse the message body.
                    CliCommand cmd;
                    bool parse_ok = cmd.ParseFromArray(
                        state.read_buffer.data() + kHpacHeaderSize,
                        static_cast<int>(msg_len));

                    // Encode response as HPAC Frame.
                    CliResponse resp;
                    if (parse_ok) {
                        resp = execute_cli_command(cmd);
                    } else {
                        resp.set_content_type("application/x-protobuf");
                        resp.set_is_error(true);
                        resp.set_error_code(1);
                    }
                    std::string wire = resp.SerializeAsString();
                    uint32_t payload_len = static_cast<uint32_t>(wire.size());
                    uint32_t net_len = htonl(payload_len);
                    ::write(client_fd, kHpacMagic, 4);
                    ::write(client_fd, &net_len, 4);
                    ::write(client_fd, wire.data(), wire.size());
                    state.read_buffer.erase(0, kHpacHeaderSize + msg_len);

                    if (!parse_ok) {
                        close_proto_session(client_fd);
                        return;
                    }
                }
            }
        } else if (n == 0) {
            closed = true;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            closed = true;
        }
    }

    if (closed) {
        close_proto_session(client_fd);
    }
}

void CliServerActor::close_proto_session(int client_fd) {
    auto it = proto_sessions_.find(client_fd);
    if (it == proto_sessions_.end())
        return;

    loop_->clear_read_handler(client_fd);
    ::close(client_fd);
    proto_sessions_.erase(it);
}

// ---------------------------------------------------------------------------
// Proto command dispatch
// ---------------------------------------------------------------------------

CliResponse CliServerActor::execute_cli_command(const CliCommand& cmd) {
    CliResponse resp;

    // Dual dispatch: rpc_method takes priority over path.
    if (!cmd.rpc_method().empty()) {
        std::string rpc_result = dispatch_rpc(cmd.rpc_method(), cmd.rpc_request());
        resp.set_content_type("application/x-protobuf");
        resp.set_payload(rpc_result);
        resp.set_is_structured(true);
        if (rpc_result.empty()) {
            resp.set_is_error(true);
            resp.set_error_code(2); // method not found
        }
        return resp;
    }

    // Command-tree dispatch.
    if (cmd.path().empty()) {
        resp.set_content_type("text/plain");
        resp.set_payload("Error: no path or rpc_method specified\n");
        resp.set_is_error(true);
        resp.set_error_code(1);
        return resp;
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
    auto formatter = OutputFormatter::create(
        cmd.format().empty() ? config_.default_format : cmd.format());
    auto temp_formatter = OutputFormatter::create(
        cmd.format().empty() ? config_.default_format : cmd.format());
    std::string* buf_ptr = &output_buffer;
    auto output_fn = [buf_ptr](const std::string& text) { *buf_ptr += text; };

    // Create a temporary session to process the command.
    auto temp_session = std::make_unique<CliSession>(
        &system_, command_tree_.get(), std::move(temp_formatter), output_fn,
        config_.page_size);
    temp_session->set_cli_server_actor(this);
    temp_session->set_command_host(this);
    temp_session->set_system_host(this);
    temp_session->set_lifecycle_host(this);

    temp_session->process_line(command_line);

    std::string format =
        cmd.format().empty() ? config_.default_format : cmd.format();
    if (format == "json") {
        resp.set_content_type("application/json");
    } else {
        resp.set_content_type("text/plain");
    }
    resp.set_payload(output_buffer);
    resp.set_is_structured(false);

    return resp;
}

std::string CliServerActor::dispatch_rpc(const std::string& rpc_method,
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
        // enumerate doesn't take a request proto, but we parse the list actors
        // request for the optional filter.
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
        // Use a StringFormatter to capture system stats as text.
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
