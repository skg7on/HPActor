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

#pragma once

#include <hpactor/actor/daemon_actor.hpp>
#include <hpactor/cli/cli_client_config.hpp>
#include <hpactor/cli/cli_command_host.hpp>

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace hpactor {

class ActorSystem;

namespace net {
class ConnectionPool;
class Connection;
} // namespace net

namespace cli {

class CliSession;
class LineEditor;
class OutputFormatter;
struct CommandNode;

/// \brief Remote CLI client actor that connects to a CliServerActor over
///        the network.
///
/// Runs as a DaemonActor on a dedicated thread. Implements the three CLI
/// host interfaces (ICliCommandHost, ISystemCliHost, ILifecycleCliHost)
/// by forwarding over the CliCommand/CliResponse protobuf wire protocol
/// rather than operating on local actor-system state.
///
/// Supports two modes:
/// - Interactive: persistent connection with LineEditor for user input.
/// - Exec: single-command mode (connect, send, receive, disconnect, exit).
///
/// \note Thread affinity: runs on a dedicated daemon thread. All public
///       methods are called from that thread unless noted otherwise.
class CliClientActor : public DaemonActor,
                       public ICliCommandHost,
                       public ISystemCliHost,
                       public ILifecycleCliHost {
  public:
    /// \brief Actor type name for CLI introspection and actor listing.
    static constexpr const char* kActorTypeName = "CliClientActor";

    /// \brief Construct the remote CLI client actor.
    ///
    /// \param[in] ctx Actor context.
    /// \param[in] system The actor system (for DaemonActor infrastructure).
    /// \param[in] config Client configuration (transport, session, timing).
    CliClientActor(ActorContext* ctx, ActorSystem& system,
                   const CliClientConfig& config);

    /// \brief Destructor (needed for unique_ptr members with incomplete types).
    ~CliClientActor() override;

    // --- DaemonActor interface ---

    /// \brief Process one iteration of the CLI input loop.
    ///
    /// In exec mode: connects, processes the single command, disconnects,
    /// and returns false. In interactive mode: ensures a connection, reads
    /// a line, processes it, and returns the running state.
    ///
    /// \retval true  Continue the daemon loop.
    /// \retval false Shut down the CLI daemon.
    bool run_once() override;

    /// \brief Called when the daemon thread starts.
    ///
    /// Builds the command tree from the registry, initializes the line
    /// editor and CliSession, and prints the greeting banner.
    void on_daemon_start() override;

    /// \brief Called when the daemon thread stops.
    ///
    /// Disconnects from the server and saves command history.
    void on_daemon_stop() override;

    /// \brief CLI client is always a system actor.
    bool is_system_actor() const override {
        return true;
    }

    // --- ICliCommandHost interface ---

    /// \brief Inspect an actor on the remote server via RPC dispatch.
    ///
    /// Serializes the request into a CliCommand with rpc_method="inspect",
    /// sends it over the wire, and deserializes the structured reply.
    ///
    /// \param[in] target Actor to inspect on the remote system.
    /// \param[in] req The inspect request.
    /// \param[in] timeout Maximum time to wait for a reply.
    /// \return The reply if received within the timeout, otherwise
    ///         \c std::nullopt.
    std::optional<class InspectStateReply>
    inspect(ActorId target, const class InspectStateRequest& req,
            std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) override;

    /// \brief Kill an actor on the remote server via RPC dispatch.
    ///
    /// \param[in] target Actor to kill on the remote system.
    /// \param[in] req The kill request.
    /// \param[in] timeout Maximum time to wait for a reply.
    /// \return The reply if received within the timeout, otherwise
    ///         \c std::nullopt.
    std::optional<class KillReply>
    kill(ActorId target, const class KillRequest& req,
         std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) override;

    /// \brief Quarantine or unquarantine an actor on the remote server via
    ///        RPC dispatch.
    ///
    /// \param[in] target Actor to quarantine/unquarantine.
    /// \param[in] req The quarantine request.
    /// \param[in] timeout Maximum time to wait for a reply.
    /// \return The reply if received within the timeout, otherwise
    ///         \c std::nullopt.
    std::optional<class QuarantineReply>
    quarantine(ActorId target, const class QuarantineRequest& req,
               std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) override;

    /// \brief Enumerate actors on the remote server via RPC dispatch.
    ///
    /// \param[in] filter Optional substring filter on actor type or behavior
    ///                   name. Empty string matches all actors.
    /// \return Metadata for each matching actor.
    std::vector<ActorMeta> enumerate(std::string_view filter = "") override;

    // --- ISystemCliHost interface ---

    /// \brief Render system stats by forwarding to the remote server.
    ///
    /// Sends a CliCommand with path="system/stats" and writes the response
    /// payload to the output formatter.
    void render_system_stats(OutputFormatter& output) override;

    /// \brief Render memory stats by forwarding to the remote server.
    void render_memory_stats(OutputFormatter& output) override;

    /// \brief Render fault status by forwarding to the remote server.
    void render_fault_status(OutputFormatter& output) override;

    /// \brief Render DLQ list by forwarding to the remote server.
    void render_dlq_list(OutputFormatter& output,
                         std::string_view filter = "") override;

    /// \brief Replay a DLQ record by forwarding to the remote server.
    result<void> dlq_replay(uint32_t index, ActorId target) override;

    // --- ILifecycleCliHost interface ---

    /// \brief Drain the remote system.
    result<void> drain() override;

    /// \brief Shut down the remote system.
    result<void> shutdown() override;

    /// \brief Set the command to run in exec mode (single command, then exit).
    ///
    /// \param[in] cmd The command line to execute.
    void set_exec_command(const std::string& cmd) {
        exec_cmd_ = cmd;
        exec_mode_ = true;
    }

    /// \brief Check if the actor is running.
    ///
    /// \return true if the interactive loop is still active.
    bool is_running() const {
        return running_;
    }

  private:
    /// \brief Open a connection to the remote server.
    ///
    /// Creates a raw TCP or UDS socket based on config and connects.
    /// Stores the file descriptor in conn_ (cast to Connection*).
    void connect();

    /// \brief Close the connection to the remote server.
    void disconnect();

    /// \brief Send a CliCommand and block until the CliResponse arrives.
    ///
    /// Serializes the command with a varint-length prefix, writes to the
    /// socket, then reads the response with the same varint-length framing.
    /// Blocks up to config_.request_timeout.
    ///
    /// \param[in] cmd The command to send.
    /// \return The response from the server. On error, is_error() is true.
    class CliResponse send_and_wait(const class CliCommand& cmd);

    ActorSystem& system_;
    CliClientConfig config_;
    std::unique_ptr<CliSession> session_;
    std::unique_ptr<LineEditor> line_editor_;
    std::unique_ptr<CommandNode> command_tree_;
    std::unique_ptr<net::ConnectionPool> pool_;
    net::Connection* conn_ = nullptr;
    bool running_ = true;
    bool exec_mode_ = false;
    std::string exec_cmd_;
    std::string recv_buffer_;
};

} // namespace cli
} // namespace hpactor
