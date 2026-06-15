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
#include <hpactor/cli/cli_client_actor.hpp>
#include <hpactor/cli/cli_session.hpp>
#include <hpactor/cli/command_node.hpp>
#include <hpactor/cli/command_registry.hpp>
#include <hpactor/cli/command_tree_builder.hpp>
#include <hpactor/cli/line_editor.hpp>
#include <hpactor/cli/output_formatter.hpp>
#include <hpactor/cli_messages.pb.h>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/types/types.hpp>

#include <arpa/inet.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

namespace hpactor {
namespace cli {

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

CliClientActor::CliClientActor(ActorContext* ctx, ActorSystem& system,
                               const CliClientConfig& config)
    : DaemonActor(ctx, system), system_(system), config_(config) {}

CliClientActor::~CliClientActor() = default;

// ---------------------------------------------------------------------------
// DaemonActor
// ---------------------------------------------------------------------------

static std::string resolve_history_path(const CliClientConfig& config) {
    if (!config.history_path.empty())
        return config.history_path;
    const char* home = getenv("HOME");
    if (!home)
        home = "/tmp";
    return std::string(home) + "/.hpactor_cli_history";
}

void CliClientActor::on_daemon_start() {
    // Build command tree from registry (same tree as server-side)
    command_tree_ = std::make_unique<CommandNode>("/", "CLI root");
    build_command_tree_from_registry(*command_tree_);

    LineEditorConfig editor_cfg;
    editor_cfg.history_path = resolve_history_path(config_);
    editor_cfg.history_max = config_.history_max;
    editor_cfg.multiline = false;
    line_editor_ = std::make_unique<LineEditor>(editor_cfg, command_tree_.get());
    line_editor_->load_history();

    session_ = std::make_unique<CliSession>(
        &system_, command_tree_.get(),
        OutputFormatter::create(config_.default_format),
        [](const std::string& text) { printf("%s", text.c_str()); }, 50);
    session_->set_command_host(this);
    session_->set_system_host(this);
    session_->set_lifecycle_host(this);

    printf("HPActor Remote CLI -- Connected.  Type /help for commands, /quit to exit.\n\n");
}

void CliClientActor::on_daemon_stop() {
    disconnect();
    if (line_editor_)
        line_editor_->save_history();
    printf("\n[Remote CLI session ended]\n");
}

bool CliClientActor::run_once() {
    if (exec_mode_) {
        connect();
        if (!conn_) {
            printf("Error: could not connect to server\n");
            return false;
        }
        session_->process_line(exec_cmd_);
        disconnect();
        return false;
    }

    if (!conn_) {
        connect();
        if (!conn_) {
            printf("Waiting for connection... (retrying in 1s)\n");
            std::this_thread::sleep_for(std::chrono::seconds(1));
            return running_;
        }
    }

    std::string line = line_editor_->readline("hpactor> ");
    if (line.empty()) {
        if (std::feof(stdin)) {
            printf("\nGoodbye.\n");
            running_ = false;
            return false;
        }
        return true;
    }

    line_editor_->add_history(line);
    if (!session_->process_line(line)) {
        running_ = false;
        return false;
    }
    return running_;
}

// ---------------------------------------------------------------------------
// Connection management
// ---------------------------------------------------------------------------

void CliClientActor::connect() {
    if (config_.transport == CliClientConfig::Transport::HttpJson) {
        // HTTP JSON mode: single connection per request (stateless).
        // For interactive, connect on each command.
        return;
    }

    // Protobuf binary mode: use a raw TCP/UDS socket managed directly.
    // ConnectionPool is designed for actor-to-actor messaging and expects
    // Frame-encoded protobuf with target addressing.  The CLI client sends
    // CliCommand protobuf directly with varint-length prefix framing.
    int fd = -1;
    if (!config_.host.empty()) {
        fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
            return;
        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(config_.port);
        if (inet_pton(AF_INET, config_.host.c_str(), &addr.sin_addr) != 1) {
            ::close(fd);
            return;
        }
        if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr),
                      sizeof(addr)) < 0) {
            ::close(fd);
            return;
        }
    } else {
        fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0)
            return;
        struct sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, config_.uds_path.c_str(), sizeof(addr.sun_path) - 1);
        if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr),
                      sizeof(addr)) < 0) {
            ::close(fd);
            return;
        }
    }
    conn_ = reinterpret_cast<net::Connection*>(static_cast<intptr_t>(fd));
}

void CliClientActor::disconnect() {
    if (conn_) {
        int fd = static_cast<int>(reinterpret_cast<intptr_t>(conn_));
        ::shutdown(fd, SHUT_RDWR);
        ::close(fd);
        conn_ = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Wire protocol — varint-length-prefixed protobuf send / receive
// ---------------------------------------------------------------------------

static void write_varint_prefixed(int fd, const std::string& data) {
    uint32_t len = static_cast<uint32_t>(data.size());
    uint8_t len_buf[5];
    int len_bytes = 0;
    uint32_t tmp = len;
    while (tmp > 0x7f) {
        len_buf[len_bytes++] = static_cast<uint8_t>(tmp & 0x7f) | 0x80;
        tmp >>= 7;
    }
    len_buf[len_bytes++] = static_cast<uint8_t>(tmp);
    ::write(fd, len_buf, static_cast<size_t>(len_bytes));
    ::write(fd, data.data(), data.size());
}

CliResponse CliClientActor::send_and_wait(const CliCommand& cmd) {
    CliResponse resp;
    resp.set_is_error(true);
    resp.set_error_code(-1);

    int fd = conn_ ? static_cast<int>(reinterpret_cast<intptr_t>(conn_)) : -1;
    if (fd < 0)
        return resp;

    // Send: varint-length prefix + serialized CliCommand
    std::string wire = cmd.SerializeAsString();
    write_varint_prefixed(fd, wire);

    // Read: varint-length prefix + serialized CliResponse
    char read_buf[4096];
    std::string accum;
    auto deadline = std::chrono::steady_clock::now() + config_.request_timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        ssize_t n = ::read(fd, read_buf, sizeof(read_buf));
        if (n > 0) {
            accum.append(read_buf, static_cast<size_t>(n));
        } else if (n == 0) {
            break; // EOF
        } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
            break; // error
        }

        // Try to decode a frame from the accumulator
        if (accum.size() >= 1) {
            uint32_t msg_len = 0;
            unsigned shift = 0;
            size_t pos = 0;
            while (pos < accum.size() && pos < 5) {
                uint8_t byte = static_cast<uint8_t>(accum[pos]);
                msg_len |= static_cast<uint32_t>(byte & 0x7f) << shift;
                pos++;
                if (!(byte & 0x80))
                    break;
                shift += 7;
            }
            if (pos > 0 && pos <= 5 && accum.size() >= pos + msg_len) {
                if (resp.ParseFromArray(accum.data() + pos,
                                        static_cast<int>(msg_len))) {
                    return resp;
                }
                // Parse failed — discard this frame and try again
                accum.erase(0, pos + msg_len);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return resp;
}

// ---------------------------------------------------------------------------
// ICliCommandHost — RPC dispatch (rpc_method + serialized request)
// ---------------------------------------------------------------------------

std::optional<InspectStateReply>
CliClientActor::inspect(ActorId target, const InspectStateRequest& req,
                        std::chrono::milliseconds /*timeout*/) {
    CliCommand cmd;
    cmd.set_rpc_method("inspect");
    InspectStateRequest mutable_req = req;
    mutable_req.set_target_actor_id(target.value());
    cmd.set_rpc_request(mutable_req.SerializeAsString());
    auto resp = send_and_wait(cmd);
    if (resp.is_structured() && !resp.is_error()) {
        InspectStateReply reply;
        if (reply.ParseFromString(resp.payload()))
            return reply;
    }
    return std::nullopt;
}

std::optional<KillReply>
CliClientActor::kill(ActorId target, const KillRequest& req,
                     std::chrono::milliseconds /*timeout*/) {
    CliCommand cmd;
    cmd.set_rpc_method("kill");
    KillRequest mutable_req = req;
    mutable_req.set_target_actor_id(target.value());
    cmd.set_rpc_request(mutable_req.SerializeAsString());
    auto resp = send_and_wait(cmd);
    if (resp.is_structured() && !resp.is_error()) {
        KillReply reply;
        if (reply.ParseFromString(resp.payload()))
            return reply;
    }
    return std::nullopt;
}

std::optional<QuarantineReply>
CliClientActor::quarantine(ActorId target, const QuarantineRequest& req,
                           std::chrono::milliseconds /*timeout*/) {
    CliCommand cmd;
    cmd.set_rpc_method("quarantine");
    QuarantineRequest mutable_req = req;
    mutable_req.set_target_actor_id(target.value());
    cmd.set_rpc_request(mutable_req.SerializeAsString());
    auto resp = send_and_wait(cmd);
    if (resp.is_structured() && !resp.is_error()) {
        QuarantineReply reply;
        if (reply.ParseFromString(resp.payload()))
            return reply;
    }
    return std::nullopt;
}

std::vector<ActorMeta> CliClientActor::enumerate(std::string_view filter) {
    CliCommand cmd;
    cmd.set_rpc_method("enumerate");
    cmd.set_rpc_request(std::string(filter));
    auto resp = send_and_wait(cmd);
    std::vector<ActorMeta> result;
    if (resp.is_structured() && !resp.is_error()) {
        ListActorsReply list_reply;
        if (list_reply.ParseFromString(resp.payload())) {
            for (const auto& pb_meta : list_reply.actors()) {
                ActorMeta m;
                m.actor_id = pb_meta.actor_id();
                m.actor_type = pb_meta.actor_type();
                m.state = pb_meta.state();
                m.incarnation = pb_meta.incarnation();
                m.messages_processed = pb_meta.messages_processed();
                m.uptime_ms = pb_meta.uptime_ms();
                m.behavior_name = pb_meta.behavior_name();
                result.push_back(std::move(m));
            }
        }
    }
    return result;
}

// ---------------------------------------------------------------------------
// ISystemCliHost — command-tree dispatch (path + args)
// ---------------------------------------------------------------------------

void CliClientActor::render_system_stats(OutputFormatter& output) {
    CliCommand cmd;
    cmd.set_path("system/stats");
    auto resp = send_and_wait(cmd);
    if (!resp.is_error())
        output.raw(resp.payload());
    else
        output.error("Failed to fetch system stats from server");
}

void CliClientActor::render_memory_stats(OutputFormatter& output) {
    CliCommand cmd;
    cmd.set_path("system/memory");
    auto resp = send_and_wait(cmd);
    if (!resp.is_error())
        output.raw(resp.payload());
    else
        output.error("Failed to fetch memory stats from server");
}

void CliClientActor::render_fault_status(OutputFormatter& output) {
    CliCommand cmd;
    cmd.set_path("fault/status");
    auto resp = send_and_wait(cmd);
    if (!resp.is_error())
        output.raw(resp.payload());
    else
        output.error("Failed to fetch fault status from server");
}

void CliClientActor::render_dlq_list(OutputFormatter& output,
                                     std::string_view filter) {
    CliCommand cmd;
    cmd.set_path("dlq/list");
    if (!filter.empty())
        cmd.add_args(std::string(filter));
    auto resp = send_and_wait(cmd);
    if (!resp.is_error())
        output.raw(resp.payload());
    else
        output.error("Failed to fetch DLQ list from server");
}

result<void> CliClientActor::dlq_replay(uint32_t index, ActorId target) {
    CliCommand cmd;
    cmd.set_path("dlq/replay");
    cmd.add_args(std::to_string(index));
    cmd.add_args(std::to_string(target.value()));
    auto resp = send_and_wait(cmd);
    if (resp.is_error())
        return result<void>::make(error(errors::unknown, "dlq replay failed"));
    return result<void>::make();
}

// ---------------------------------------------------------------------------
// ILifecycleCliHost — command-tree dispatch
// ---------------------------------------------------------------------------

result<void> CliClientActor::drain() {
    CliCommand cmd;
    cmd.set_path("system/drain");
    auto resp = send_and_wait(cmd);
    if (resp.is_error())
        return result<void>::make(error(errors::unknown, "drain failed"));
    return result<void>::make();
}

result<void> CliClientActor::shutdown() {
    CliCommand cmd;
    cmd.set_path("quit");
    auto resp = send_and_wait(cmd);
    if (resp.is_error())
        return result<void>::make(error(errors::unknown, "shutdown failed"));
    return result<void>::make();
}

} // namespace cli
} // namespace hpactor
