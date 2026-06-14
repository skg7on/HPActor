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
namespace cli {
class CliSession;
struct CommandNode;

/// \brief Socket-based CLI server operating as a daemon actor.
///
/// Listens on Unix domain socket and/or TCP port, accepts connections,
/// and processes CLI commands through the same command tree as CliActor.
/// Each connection is wrapped in a CliSession for transport-agnostic
/// command dispatch.
///
/// Runs on a dedicated daemon thread via DispatchPolicy::DedicatedThread.
///
/// \note Thread affinity: runs on a dedicated daemon thread. All public
///       methods are called from that thread unless noted otherwise.
class CliServerActor : public DaemonActor {
  public:
    /// \brief Actor type name for CLI introspection and actor listing.
    static constexpr const char* kActorTypeName = "CliServerActor";

    /// \brief Construct the CLI server actor.
    ///
    /// \param[in] ctx Actor context.
    /// \param[in] system The actor system.
    /// \param[in] config CLI server configuration.
    CliServerActor(ActorContext* ctx, ActorSystem& system,
                   const CliServerConfig& config);

    /// \brief Destructor.
    ~CliServerActor() override;

    // --- DaemonActor interface ---

    /// \brief Process one iteration of the daemon loop.
    ///
    /// Accepts new connections, services existing sessions, and
    /// removes idle / dead sessions.
    ///
    /// \retval true Continue the daemon loop.
    /// \retval false Shut down the CLI server.
    bool run_once() override;

    /// \brief Called when the daemon thread starts.
    ///
    /// Binds listeners and builds the command tree.
    void on_daemon_start() override;

    /// \brief Called when the daemon thread stops.
    ///
    /// Closes all sessions and listeners.
    void on_daemon_stop() override;

    /// \brief CLI server is always a system actor.
    bool is_system_actor() const override {
        return true;
    }

    /// \brief Return metadata for CLI introspection.
    cli::ActorMeta to_metadata() const override;

    // --- Accessors for command handlers ---

    /// \brief Reference to the actor system.
    ActorSystem& system() {
        return system_;
    }

    /// \brief Read-only access to the command tree.
    const CommandNode* command_tree() const {
        return command_tree_.get();
    }

    /// \brief Request the CLI server to shut down.
    void request_shutdown() {
        running_ = false;
    }

    // --- Request-Response Helpers ---

    /// \brief Send an InspectStateRequest and block on the reply.
    std::optional<class InspectStateReply> send_and_wait_inspect(
        ActorId target, const class InspectStateRequest& req,
        std::chrono::milliseconds timeout = std::chrono::milliseconds(2000));

    /// \brief Send a KillRequest and block on the reply.
    std::optional<class KillReply> send_and_wait_kill(
        ActorId target, const class KillRequest& req,
        std::chrono::milliseconds timeout = std::chrono::milliseconds(2000));

    /// \brief Send a QuarantineRequest and block on the reply.
    std::optional<class QuarantineReply> send_and_wait_quarantine(
        ActorId target, const class QuarantineRequest& req,
        std::chrono::milliseconds timeout = std::chrono::milliseconds(2000));

    /// \brief Enumerate all known actors.
    std::vector<ActorMeta> enumerate_actors(const std::string& filter = "");

  private:
    /// \brief Bind UDS and TCP listeners.
    ///
    /// \return result<void> with error on failure.
    result<void> bind_listeners();

    /// \brief Accept new connections on all listeners.
    void accept_connections();

    /// \brief Read and process input from all active sessions.
    void service_sessions();

    /// \brief Remove sessions idle past the configured timeout.
    void remove_dead_sessions();

    /// \brief Close and remove a session by its index.
    void close_session(size_t index);

    /// \brief Build the command tree from the CommandRegistry.
    void build_command_tree();

    // --- helpers ---

    /// \brief Create a non-blocking, close-on-exec socket.
    ///
    /// \param[in] domain Socket domain (e.g. AF_UNIX, AF_INET).
    /// \param[in] type Socket type (e.g. SOCK_STREAM).
    /// \return File descriptor on success, or -1 with errno set.
    static int make_nonblocking_socket(int domain, int type);

    /// \brief Accept a connection with CLOEXEC and NONBLOCK flags.
    ///
    /// Portable across Linux (accept4) and macOS (accept + fcntl).
    ///
    /// \param[in] listen_fd Listening socket file descriptor.
    /// \return Client fd on success, or -1 with errno set.
    static int portable_accept(int listen_fd);

    ActorSystem& system_;
    CliServerConfig config_;

    int uds_listen_fd_ = -1;
    int tcp_listen_fd_ = -1;
    std::unique_ptr<CommandNode> command_tree_;
    bool running_ = true;

    /// \brief Per-session state.
    struct SessionState {
        int fd = -1;
        std::unique_ptr<CliSession> session;
        std::chrono::steady_clock::time_point last_activity;
        std::string read_buffer;
    };
    std::vector<SessionState> sessions_;
};

} // namespace cli
} // namespace hpactor
