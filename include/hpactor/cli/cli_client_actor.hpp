// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <hpactor/cli/cli_client_config.hpp>
#include <hpactor/cli/cli_connector.hpp>
#include <hpactor/cli/interactive_cli_actor.hpp>

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace hpactor {

class ActorSystem;

namespace cli {

/// \brief Remote CLI client connecting to a CliProtoServerActor via UDS or TCP.
///
/// Implements the three CLI host interfaces by serializing requests to
/// \c CliCommand protobuf messages as HPAC Frames and sending them
/// through \c Connection::send().  Connection establishment uses
/// \c CliConnector for non-blocking async connect via EventLoop.
///
/// Supports two modes:
/// - Interactive: persistent connection with LineEditor for user input.
/// - Exec: single-command mode (connect, send, receive, disconnect, exit).
class CliClientActor : public InteractiveCliActor {
  public:
    static constexpr const char* kActorTypeName = "CliClientActor";

    CliClientActor(ActorContext* ctx, ActorSystem& system,
                   const CliClientConfig& config);
    ~CliClientActor() override;

    // ICliCommandHost — structured RPC dispatch (rpc_method + rpc_request)
    std::optional<class InspectStateReply>
    inspect(ActorId target, const class InspectStateRequest& req,
            std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) override;
    std::optional<class KillReply>
    kill(ActorId target, const class KillRequest& req,
         std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) override;
    std::optional<class QuarantineReply>
    quarantine(ActorId target, const class QuarantineRequest& req,
               std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) override;
    std::vector<ActorMeta> enumerate(std::string_view filter = "") override;

    // ISystemCliHost — command-tree dispatch (path + args)
    bool execute_path(std::string_view path,
                      const std::map<std::string, std::string>& params,
                      const std::vector<std::string>& args,
                      OutputFormatter& output) override;
    result<void> dlq_replay(uint32_t index, ActorId target) override;

    // ILifecycleCliHost
    result<void> drain() override;
    result<void> shutdown() override;

    /// \brief Set the command to run in exec mode (single command, then exit).
    void set_exec_command(const std::string& cmd) {
        exec_cmd_ = cmd;
        exec_mode_ = true;
    }

    // ── Client-side /client command support ──────────────────────────
    /// \brief Return connection status for this client (self only).
    std::string list_clients() const;
    /// \brief Disconnect this client from the server.
    bool close_client(uint32_t seqno);
    /// \brief Return own command history (not tracked locally).
    std::string client_history(uint32_t seqno) const;

  protected:
    // InteractiveCliActor virtual hooks
    void print_greeting() override;
    void print_farewell() override;
    std::string get_history_path() override;
    uint32_t get_history_max() override;
    std::string get_default_format() override;
    uint32_t get_page_size() override;
    bool pre_input_hook() override;
    void pre_stop_hook() override;
    void on_session_wired(CliSession& session) override;

  private:
    void connect();
    void disconnect();
    class CliResponse send_and_wait(const class CliCommand& cmd);

    CliClientConfig config_;
    class CliConnector connector_;
    bool exec_mode_ = false;
    bool was_ever_connected_ = false;
    bool intentionally_disconnected_ = false;
    std::string exec_cmd_;
};

} // namespace cli
} // namespace hpactor
