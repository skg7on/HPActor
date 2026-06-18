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
#include <hpactor/cli/cli_connector.hpp>
#include <hpactor/cli/cli_session.hpp>
#include <hpactor/cli/output_formatter.hpp>
#include <hpactor/cli_messages.pb.h>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/types/types.hpp>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <sys/socket.h>
#include <thread>

#include <hpactor/msg/frame.hpp>
#include <hpactor/net/wireframe_connection.hpp>

namespace hpactor {
namespace cli {

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

CliClientActor::CliClientActor(ActorContext* ctx, ActorSystem& system,
                               const CliClientConfig& config)
    : InteractiveCliActor(ctx, system), config_(config) {}

CliClientActor::~CliClientActor() = default;

// ---------------------------------------------------------------------------
// InteractiveCliActor virtual hooks
// ---------------------------------------------------------------------------

void CliClientActor::print_greeting() {
    // Wire the client pointer into CliSession so /client commands can detect
    // they're running on a client and show self-only information.
    session_->set_client_actor(this);
    // Defer the real greeting to pre_input_hook() — the connection hasn't been
    // attempted yet at this point.  Don't claim "Connected" or suggest typing
    // commands until the transport is actually up.
}

void CliClientActor::print_farewell() {
    printf("\n[Remote CLI session ended]\n");
}

std::string CliClientActor::get_history_path() {
    if (!config_.history_path.empty())
        return config_.history_path;
    const char* home = getenv("HOME");
    if (!home)
        home = "/tmp";
    return std::string(home) + "/.hpactor_cli_history";
}

uint32_t CliClientActor::get_history_max() {
    return config_.history_max;
}

std::string CliClientActor::get_default_format() {
    return config_.default_format;
}

uint32_t CliClientActor::get_page_size() {
    return 50;
}

bool CliClientActor::pre_input_hook() {
    if (exec_mode_) {
        connect();
        if (connector_.fd() < 0) {
            printf("Error: could not connect to server\n");
            return false;
        }
        session_->process_line(exec_cmd_);
        disconnect();
        return false; // exit after exec
    }

    // Active disconnect via /client close — don't reconnect.
    if (intentionally_disconnected_) {
        printf("\n[Disconnected from server]\n");
        running_ = false;
        return false;
    }

    // Liveness check: the local ConnectionState may still report Connected
    // even after the server has closed its end of the socket.  A non-blocking
    // peek detects the TCP FIN so we can trigger the reconnection path.
    if (connector_.is_connected()) {
        char discard;
        ssize_t n = ::recv(connector_.fd(), &discard, 1, MSG_PEEK | MSG_DONTWAIT);
        // EOF (n == 0): server closed the connection cleanly.
        // Error (n < 0, not EAGAIN): hard error on the socket.
        // Both force a disconnect so the reconnection block below picks up.
        if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK))
            connector_.disconnect();
    }

    if (!connector_.is_connected()) {
        // Previously connected and now disconnected — the remote side
        // closed the connection (server shutdown, network drop, etc.).
        if (was_ever_connected_) {
            printf("\n[Server disconnected — reconnecting]\n\n");
            was_ever_connected_ = false;
        }

        connect();
        if (!connector_.is_connected()) {
            if (!was_ever_connected_) {
                if (!config_.host.empty())
                    printf("Connecting to %s:%u... (retrying every 1s)\n",
                           config_.host.c_str(),
                           static_cast<unsigned>(config_.port));
                else
                    printf("Connecting to %s... (retrying every 1s)\n",
                           config_.uds_path.c_str());
            }
            std::this_thread::sleep_for(std::chrono::seconds(1));
            return running_; // keep retrying
        }
        if (!was_ever_connected_) {
            printf("HPActor Remote CLI — Connected.  Type /help for commands, "
                   "/quit to exit.\n\n");
            was_ever_connected_ = true;
        }
    }
    return true;
}

void CliClientActor::pre_stop_hook() {
    disconnect();
}

// ---------------------------------------------------------------------------
// Connection management — delegates to CliConnector for async connect
// ---------------------------------------------------------------------------

void CliClientActor::connect() {
    if (connector_.is_connected())
        return;

    if (config_.transport == CliClientConfig::Transport::HttpJson) {
        return; // HTTP JSON mode is deferred
    }

    if (!config_.host.empty()) {
        connector_.connect_tcp(config_.host, config_.port, config_.connect_timeout);
    } else {
        connector_.connect_uds(config_.uds_path, config_.connect_timeout);
    }
}

void CliClientActor::disconnect() {
    connector_.disconnect();
}

// ---------------------------------------------------------------------------
// Wire protocol — send via Connection::send() (same path as try_send),
// receive via EventLoop read handler with HPAC Frame decoding.
// ---------------------------------------------------------------------------

namespace {

/// Encode protobuf data as an HPAC WireFrame: magic + big-endian length + data.
inline StreamBuffer encode_as_frame(const std::string& protobuf_data) {
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

CliResponse CliClientActor::send_and_wait(const CliCommand& cmd) {
    CliResponse resp;
    resp.set_is_error(true);
    resp.set_error_code(-1);

    auto* transport = connector_.transport();
    auto conn = connector_.connection();
    if (!transport || !conn || !connector_.is_connected())
        return resp;

    // 1. Encode CliCommand as an HPAC Frame and send through
    //    Connection::send() — the same method TcpTransport::try_send calls.
    std::string cmd_bytes = cmd.SerializeAsString();
    auto frame_data = encode_as_frame(cmd_bytes);
    conn->send(frame_data);

    // 2. Receive: poll the EventLoop, reading raw data from the fd
    //    through the EventLoop's read path.  We temporarily set a
    //    frame handler that captures the response body.
    auto done = std::make_shared<bool>(false);
    auto response_body = std::make_shared<std::string>();

    // Store original handler to restore later.
    // WireFrameConnection set_frame_handler replaces the handler.
    static_cast<net::WireFrameConnection*>(conn.get())
        ->set_frame_handler([done, response_body](hpactor::adt::StreamBuffer data) {
            *response_body = std::string(
                reinterpret_cast<const char*>(data.data()), data.size());
            *done = true;
        });

    // Poll the EventLoop.  WireFrameConnection::handle_read() decodes
    // incoming HPAC frames and delivers the body to the frame handler.
    //
    // Must call process_completions() before wait() so that the send
    // completion from conn->send() above drains is_sending_.  On epoll
    // the async_send pushes completions to a pending queue that is only
    // drained by process_events(); without this, is_sending_ stays true
    // and the next send_and_wait silently queues without flushing.
    auto deadline = std::chrono::steady_clock::now() + config_.request_timeout;
    while (!*done && std::chrono::steady_clock::now() < deadline) {
        transport->loop().process_completions();
        transport->loop().wait(50);
    }

    // Restore handler — the connection pool will re-register its own
    // handler on next use.
    static_cast<net::WireFrameConnection*>(conn.get())->set_frame_handler(nullptr);

    // 3. Decode the response.
    if (*done && !response_body->empty()) {
        resp.ParseFromArray(response_body->data(),
                            static_cast<int>(response_body->size()));
    }

    // 4. If the request failed (timeout or transport error), force a
    //    disconnect so pre_input_hook() detects it on the next iteration
    //    and can print a disconnection message + trigger reconnection.
    if (resp.is_error())
        connector_.disconnect();

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

bool CliClientActor::execute_path(std::string_view path,
                                  const std::map<std::string, std::string>& params,
                                  const std::vector<std::string>& args,
                                  OutputFormatter& output) {
    CliCommand cmd;
    cmd.set_path(std::string(path));
    for (const auto& [key, value] : params)
        (*cmd.mutable_params())[key] = value;
    for (const auto& arg : args)
        cmd.add_args(arg);
    auto resp = send_and_wait(cmd);
    if (!resp.is_error()) {
        output.raw(resp.payload());
        return true;
    }
    output.error("Failed to fetch data from server");
    return true;
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

// ---------------------------------------------------------------------------
// Client-side /client command support — self-only information
// ---------------------------------------------------------------------------

std::string CliClientActor::list_clients() const {
    if (connector_.is_connected()) {
        if (!config_.host.empty())
            return "Client connected to server at " + config_.host + ":" +
                   std::to_string(config_.port) + "\n";
        else
            return "Client connected to server via " + config_.uds_path + "\n";
    }
    return "Not connected to any server.\n";
}

bool CliClientActor::close_client(uint32_t /*seqno*/) {
    if (!connector_.is_connected())
        return false;
    printf("Disconnecting from server...\n");
    disconnect();
    intentionally_disconnected_ = true;
    return true;
}

std::string CliClientActor::client_history(uint32_t /*seqno*/) const {
    return "Command history is tracked on the server side.\n";
}

} // namespace cli
} // namespace hpactor
