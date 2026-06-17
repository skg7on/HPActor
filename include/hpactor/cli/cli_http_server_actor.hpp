// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <hpactor/actor/daemon_actor.hpp>
#include <hpactor/cli/cli_command_host.hpp>
#include <hpactor/cli/cli_http_server_config.hpp>

#include <memory>

namespace hpactor {

class ActorSystem;

namespace net {
class HTTPGateway;
class HTTPConnection;
} // namespace net

namespace cli {

class CliSession;
struct CommandNode;

/// \brief HTTP REST API server for the actor system, running as a daemon
/// actor.
///
/// Serves a resource-oriented REST API under \c /api/v1/ with 24 endpoints
/// across five resource groups: actors, system, faults, dead-letter-queue,
/// and asks.  Reuses \c net::HTTPGateway for HTTP listen/accept/parse and
/// response formatting.  Routes are dispatched through an internal route
/// table; each handler is a free function in \c src/cli/handlers/.
///
/// Also serves a legacy \c POST /cli endpoint (gated behind
/// \c CliHttpServerConfig::legacy_cli_endpoint) that tunnels CLI commands
/// through \c CliSession for backward compatibility.
///
/// Implements \c ICliCommandHost, \c ISystemCliHost, and
/// \c ILifecycleCliHost.  Actor operations (inspect, kill, quarantine) use
/// synchronous request-response via \c context()->send() and mailbox polling
/// with a configurable timeout.
///
/// \note Thread affinity: dedicated daemon thread via
///       \c DispatchPolicy::DedicatedThread.  The \c inspect(), \c kill(),
///       and \c quarantine() methods block the daemon thread for up to
///       \p timeout milliseconds while polling the mailbox for a reply.
///       During this window no new HTTP connections are accepted.
class CliHttpServerActor : public DaemonActor,
                           public ICliCommandHost,
                           public ISystemCliHost,
                           public ILifecycleCliHost {
  public:
    static constexpr const char* kActorTypeName = "CliHttpServerActor";

    CliHttpServerActor(ActorContext* ctx, ActorSystem& system,
                       const CliHttpServerConfig& config);
    ~CliHttpServerActor() override;

    bool run_once() override;
    void on_daemon_start() override;
    void on_daemon_stop() override;
    bool is_system_actor() const override {
        return true;
    }

    // ISystemCliHost
    void render_system_stats(OutputFormatter& output) override;
    void render_memory_stats(OutputFormatter& output) override;
    void render_fault_status(OutputFormatter& output) override;
    void render_dlq_list(OutputFormatter& output,
                         std::string_view filter = "") override;
    result<void> dlq_replay(uint32_t index, ActorId target) override;

    // ILifecycleCliHost
    result<void> drain() override;
    result<void> shutdown() override;

    // ICliCommandHost — synchronous request-response via mailbox polling

    /// \brief Inspect an actor and return its full state.
    ///
    /// Sends an \c InspectStateRequest via \c context()->send() and polls
    /// the daemon's mailbox for an \c InspectStateReply.  Uses MessageId
    /// correlation to distinguish replies from concurrent requests.
    ///
    /// \param[in] target  Actor to inspect.
    /// \param[in] req     Inspection parameters (which sections to include).
    /// \param[in] timeout Maximum time to wait for a reply (default 2000 ms).
    /// \return The inspection reply on success, \c std::nullopt on timeout
    ///         or delivery failure.
    /// \note Blocks the daemon thread while polling.  Non-matching messages
    ///       received during the polling window are discarded.
    std::optional<InspectStateReply>
    inspect(ActorId target, const InspectStateRequest& req,
            std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) override;

    /// \brief Kill (force-stop) an actor.
    ///
    /// Sends a \c KillRequest via \c context()->send() and polls for a
    /// \c KillReply.
    ///
    /// \param[in] target  Actor to kill.
    /// \param[in] req     Kill parameters (target_actor_id, force flag).
    /// \param[in] timeout Maximum time to wait for a reply (default 2000 ms).
    /// \return The kill reply on success, \c std::nullopt on timeout or
    ///         delivery failure.
    /// \note Blocks the daemon thread while polling.
    std::optional<KillReply>
    kill(ActorId target, const KillRequest& req,
         std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) override;

    /// \brief Quarantine or unquarantine an actor.
    ///
    /// Sends a \c QuarantineRequest via \c context()->send() and polls for
    /// a \c QuarantineReply.  Set \c QuarantineRequest::unquarantine to
    /// \c true to release an actor from quarantine.
    ///
    /// \param[in] target  Actor to quarantine or release.
    /// \param[in] req     Quarantine parameters (reason, unquarantine flag).
    /// \param[in] timeout Maximum time to wait for a reply (default 2000 ms).
    /// \return The quarantine reply on success, \c std::nullopt on timeout
    ///         or delivery failure.
    /// \note Blocks the daemon thread while polling.
    std::optional<QuarantineReply>
    quarantine(ActorId target, const QuarantineRequest& req,
               std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) override;

    /// \brief Enumerate all known actors, optionally filtered by type
    /// substring.
    ///
    /// Iterates the actor registry via \c ActorSystem::for_each_actor() and
    /// collects metadata for every matching actor.
    ///
    /// \param[in] filter Substring match against actor type name (empty = all).
    /// \return A vector of \c ActorMeta for every matching actor.  May be
    ///         empty if no actors match or the registry is empty.
    /// \note This method does not block — it reads the registry directly.
    std::vector<ActorMeta> enumerate(std::string_view filter = "") override;

    // Accessors for handlers

    /// \brief Return the server's configuration (read-only).
    const CliHttpServerConfig& config() const {
        return config_;
    }
    /// \brief Return the command tree for legacy \c POST /cli dispatch.
    CommandNode* command_tree() {
        return command_tree_.get();
    }
    /// \brief Return the actor system reference.
    ActorSystem& system() {
        return system_;
    }

    /// \brief Request the daemon loop to exit at the next \c run_once()
    /// iteration.
    void request_shutdown() {
        running_ = false;
    }

  private:
    void build_command_tree();
    void dispatch_route(net::HTTPConnection* conn, net::HttpRequest&& req);
    void init_routes();

    // PIMPL for route table storage (avoids exposing src/cli/handlers types
    // in this public header).
    struct RouteTable;
    std::unique_ptr<RouteTable> route_table_;

    ActorSystem& system_;
    CliHttpServerConfig config_;
    std::unique_ptr<net::HTTPGateway> gateway_;
    std::unique_ptr<CommandNode> command_tree_;
    bool running_ = true;
    bool listen_ok_ = false;
};

} // namespace cli
} // namespace hpactor
