// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <hpactor/actor/daemon_actor.hpp>
#include <hpactor/cli/cli_command_host.hpp>
#include <hpactor/cli/cli_http_server_config.hpp>

#include <memory>
#include <string>

namespace hpactor {

class ActorSystem;

namespace net {
class HTTPGateway;
class HTTPConnection;
} // namespace net

namespace cli {

class CliSession;
struct CommandNode;

/// \brief HTTP JSON CLI server running as a daemon actor.
///
/// Reuses \c net::HTTPGateway for HTTP listen/accept/parse/response formatting.
/// Accepts \c POST /cli with a JSON-encoded \c CliCommand body, routes the
/// reconstructed command line through \c CliSession, and returns a
/// JSON-encoded \c CliResponse.
///
/// Implements \c ICliCommandHost, \c ISystemCliHost, and \c ILifecycleCliHost
/// so that actor-level operations (inspect, kill, quarantine, enumerate) and
/// system-level commands (stats, memory, faults, DLQ, drain, shutdown) work
/// over HTTP.
///
/// \note Thread affinity: dedicated daemon thread via
///       \c DispatchPolicy::DedicatedThread.
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
    std::optional<InspectStateReply>
    inspect(ActorId target, const InspectStateRequest& req,
            std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) override;

    std::optional<KillReply>
    kill(ActorId target, const KillRequest& req,
         std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) override;

    std::optional<QuarantineReply>
    quarantine(ActorId target, const QuarantineRequest& req,
               std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) override;

    std::vector<ActorMeta> enumerate(std::string_view filter = "") override;

    // Accessors for handlers
    const CliHttpServerConfig& config() const {
        return config_;
    }
    CommandNode* command_tree() {
        return command_tree_.get();
    }
    ActorSystem& system() {
        return system_;
    }

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
