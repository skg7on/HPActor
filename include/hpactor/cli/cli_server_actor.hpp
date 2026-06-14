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
#include <hpactor/cli/cli_server_config.hpp>
#include <hpactor/cli/cli_types.hpp>

#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace hpactor {

class ActorSystem;

namespace net {
class EventLoop;
class TcpAcceptor;
class UnixDomainAcceptor;
class HTTPConnection;
using HTTPConnectionPtr = std::shared_ptr<HTTPConnection>;
} // namespace net

namespace cli {

class CliSession;
struct CommandNode;

/// \brief Socket-based CLI server operating as a daemon actor.
///
/// Uses \c net::TcpAcceptor and \c net::UnixDomainAcceptor for non-blocking
/// listen/accept via a dedicated EventLoop — the same pattern as
/// \c net::HTTPGateway.  Each accepted connection is wrapped in a
/// \c CliSession for transport-agnostic command dispatch.
///
/// Client fd I/O is event-driven: read_handlers registered with the
/// EventLoop drain data into per-session buffers, split complete lines,
/// and dispatch them through \c CliSession::process_line().
///
/// Runs on a dedicated daemon thread via \c DispatchPolicy::DedicatedThread.
class CliServerActor : public DaemonActor {
  public:
    static constexpr const char* kActorTypeName = "CliServerActor";

    CliServerActor(ActorContext* ctx, ActorSystem& system,
                   const CliServerConfig& config);

    ~CliServerActor() override;

    // --- DaemonActor interface ---
    bool run_once() override;
    void on_daemon_start() override;
    void on_daemon_stop() override;
    bool is_system_actor() const override {
        return true;
    }

    /// \brief Return metadata for CLI introspection.
    cli::ActorMeta to_metadata() const override;

    // --- Accessors for command handlers ---
    ActorSystem& system() {
        return system_;
    }
    const CommandNode* command_tree() const {
        return command_tree_.get();
    }
    void request_shutdown() {
        running_ = false;
    }

    // --- Request-Response Helpers ---
    std::optional<class InspectStateReply> send_and_wait_inspect(
        ActorId target, const class InspectStateRequest& req,
        std::chrono::milliseconds timeout = std::chrono::milliseconds(2000));
    std::optional<class KillReply> send_and_wait_kill(
        ActorId target, const class KillRequest& req,
        std::chrono::milliseconds timeout = std::chrono::milliseconds(2000));
    std::optional<class QuarantineReply> send_and_wait_quarantine(
        ActorId target, const class QuarantineRequest& req,
        std::chrono::milliseconds timeout = std::chrono::milliseconds(2000));
    std::vector<ActorMeta> enumerate_actors(const std::string& filter = "");

  private:
    /// Called by acceptors when a new client connects.
    void on_client_accepted(int client_fd);

    /// Called by the EventLoop when a client fd is readable.
    void on_client_readable(int client_fd);

    /// Close a session and remove it from the session table.
    void close_session(int client_fd);

    /// Build the command tree from the CommandRegistry.
    void build_command_tree();

    ActorSystem& system_;
    CliServerConfig config_;

    /// Dedicated EventLoop driving all I/O (acceptors + client fds).
    std::unique_ptr<net::EventLoop> loop_;

    /// Acceptors (owned; may be null if the corresponding transport is
    /// disabled in config).
    std::unique_ptr<net::TcpAcceptor> tcp_acceptor_;
    std::unique_ptr<net::UnixDomainAcceptor> uds_acceptor_;

    std::unique_ptr<CommandNode> command_tree_;
    bool running_ = true;

    /// Per-session state, keyed by client fd.
    struct SessionState {
        std::unique_ptr<CliSession> session;
        std::chrono::steady_clock::time_point last_activity;
        std::string read_buffer;
    };
    /// Map from client fd → SessionState.  Used by the read_handler
    /// callback to locate the owning session.
    std::unordered_map<int, SessionState> sessions_;
};

} // namespace cli
} // namespace hpactor
