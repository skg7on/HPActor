// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <hpactor/actor/daemon_actor.hpp>
#include <hpactor/cli/cli_command_host.hpp>
#include <hpactor/cli/cli_proto_server_config.hpp>
#include <hpactor/cli/cli_types.hpp>

#include <chrono>
#include <memory>
#include <string>
#include <unordered_map>

namespace hpactor {

class ActorSystem;

namespace net {
class EventLoop;
class TcpAcceptor;
class UnixDomainAcceptor;
class WireFrameConnection;
using WireFrameConnectionPtr = std::shared_ptr<WireFrameConnection>;
} // namespace net

namespace cli {

class CliSession;
struct CommandNode;

/// \brief Protobuf CLI server operating as a daemon actor.
///
/// Uses \c net::TcpAcceptor and \c net::UnixDomainAcceptor for non-blocking
/// listen/accept via a dedicated EventLoop.  Each accepted connection is
/// wrapped in a \c net::WireFrameConnection for HPAC Frame auto-decode/encode,
/// and paired with a \c CliSession for transport-agnostic command dispatch.
///
/// Incoming \c CliCommand protobuf messages are decoded from HPAC Frames and
/// dispatched through the command tree or structured RPC.  Responses are
/// encoded back as HPAC Frames and sent through the WireFrameConnection.
///
/// Implements \c ICliCommandHost, \c ISystemCliHost, and \c ILifecycleCliHost
/// by querying the local \c ActorSystem directly.
///
/// Runs on a dedicated daemon thread via \c DispatchPolicy::DedicatedThread.
class CliProtoServerActor : public DaemonActor,
                            public ICliCommandHost,
                            public ISystemCliHost,
                            public ILifecycleCliHost {
  public:
    static constexpr const char* kActorTypeName = "CliProtoServerActor";

    CliProtoServerActor(ActorContext* ctx, ActorSystem& system,
                        const CliProtoServerConfig& config);
    ~CliProtoServerActor() override;

    // --- DaemonActor interface ---
    bool run_once() override;
    void on_daemon_start() override;
    void on_daemon_stop() override;
    bool is_system_actor() const override {
        return true;
    }

    /// \brief Return metadata for CLI introspection.
    cli::ActorMeta to_metadata() const override;

    // --- Accessors ---
    void request_shutdown() {
        running_ = false;
    }

    // --- ICliCommandHost interface ---
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

    // --- ISystemCliHost interface ---
    void render_system_stats(OutputFormatter& output) override;
    void render_memory_stats(OutputFormatter& output) override;
    void render_fault_status(OutputFormatter& output) override;
    void render_scheduler_workers(OutputFormatter& output) override;
    void render_metrics_show(OutputFormatter& output) override;
    void render_dlq_list(OutputFormatter& output,
                         std::string_view filter = "") override;
    result<void> dlq_replay(uint32_t index, ActorId target) override;

    // --- ILifecycleCliHost interface ---
    result<void> drain() override;
    result<void> shutdown() override;

  private:
    /// Build the command tree from the CommandRegistry.
    void build_command_tree();

    // --- Accept handlers ---
    void on_tcp_accepted(int client_fd, EndPoint remote_ep);
    void on_uds_accepted(int client_fd);

    // --- Frame handler for decoded HPAC frames ---
    void on_frame_received(int client_fd, adt::StreamBuffer data);

    // --- Encode + send an HPAC Frame response ---
    void send_hpac_frame(int client_fd, const std::string& data);

    // --- Structured RPC dispatch ---
    std::string
    dispatch_rpc(const std::string& method, const std::string& request_bytes);

    // --- Close a proto session ---
    void close_proto_session(int client_fd);

    ActorSystem& system_;
    CliProtoServerConfig config_;
    std::unique_ptr<net::EventLoop> loop_;
    std::unique_ptr<net::TcpAcceptor> tcp_acceptor_;
    std::unique_ptr<net::UnixDomainAcceptor> uds_acceptor_;
    std::unique_ptr<CommandNode> command_tree_;
    bool running_ = true;

    struct SessionState {
        net::WireFrameConnectionPtr conn;
        std::unique_ptr<CliSession> session;
        std::chrono::steady_clock::time_point last_activity;
    };
    std::unordered_map<int, SessionState> sessions_;
};

} // namespace cli
} // namespace hpactor
