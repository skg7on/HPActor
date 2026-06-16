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
/// Implements \c ISystemCliHost and \c ILifecycleCliHost so that system-level
/// commands (stats, memory, faults, DLQ, drain, shutdown) work over HTTP.
/// Does \e not implement \c ICliCommandHost — HTTP clients use the command-tree
/// path for all operations.
///
/// \note Thread affinity: dedicated daemon thread via
///       \c DispatchPolicy::DedicatedThread.
class CliHttpServerActor : public DaemonActor,
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

    void request_shutdown() {
        running_ = false;
    }

  private:
    void build_command_tree();
    void on_http_request(net::HTTPConnection* conn, struct net::HttpRequest&& req);

    ActorSystem& system_;
    CliHttpServerConfig config_;
    std::unique_ptr<net::HTTPGateway> gateway_;
    std::unique_ptr<CommandNode> command_tree_;
    bool running_ = true;
    bool listen_ok_ = false;
};

} // namespace cli
} // namespace hpactor
