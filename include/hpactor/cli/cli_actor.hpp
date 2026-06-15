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
#include <hpactor/cli/cli_command_host.hpp>
#include <hpactor/cli/cli_config.hpp>
#include <hpactor/cli/cli_types.hpp>
#include <hpactor/cli/command_node.hpp>
#include <hpactor/cli/line_editor.hpp>
#include <hpactor/cli/output_formatter.hpp>
#include <hpactor/cli/pager.hpp>
#include <hpactor/cli/token.hpp>
#include <hpactor/types/types.hpp>

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace hpactor {

class ActorSystem;

namespace cli {

class CliSession;

// Forward-declare protobuf types (defined in cli_messages.pb.h)
class InspectStateReply;
class KillReply;
class ListActorsReply;
class SystemStatsReply;
class MemoryStatsReply;

/// \brief Interactive CLI actor with dedicated I/O thread.
///
/// Reads commands from stdin (or a UDS/TCP socket), tokenizes input via Lexer,
/// walks the command tree, and dispatches to registered command handlers.
/// Runs on \c DispatchPolicy::DedicatedThread so it may block synchronously
/// on request-response round-trips without contending with the scheduler.
///
/// \note Thread affinity: runs on a dedicated daemon thread. All public
///       methods are called from that thread unless noted otherwise.
class CliActor : public DaemonActor,
                 public ICliCommandHost,
                 public ISystemCliHost,
                 public ILifecycleCliHost {
  public:
    /// \brief Actor type name for CLI introspection and actor listing.
    static constexpr const char* kActorTypeName = "CliActor";

    /// \brief Return metadata for CLI introspection.
    ///
    /// Overrides the base to report state from is_running() rather than
    /// falling back to "unknown" (CliActor does not use the lifecycle system).
    cli::ActorMeta to_metadata() const override {
        cli::ActorMeta m;
        m.actor_id = id().value();
        m.actor_type = std::string(type_name());
        m.state = running_ ? "Running" : "Stopped";
        return m;
    }

    /// \brief Construct the CLI actor.
    ///
    /// \param[in] ctx Actor context.
    /// \param[in] system The actor system, for sending inspect/kill/list
    /// requests.
    /// \param[in] config CLI subsystem configuration.
    CliActor(ActorContext* ctx, ActorSystem& system, const CliConfig& config);

    /// \brief Destructor (needed for unique_ptr<CliSession> with incomplete
    ///        type at declaration point).
    ~CliActor() override;

    // --- DaemonActor interface ---

    /// \brief Process one iteration of the CLI input loop.
    ///
    /// Reads a line, tokenizes, walks the command tree, and executes the
    /// matched handler. Returns false when the input stream ends or
    /// request_shutdown() is called.
    ///
    /// \retval true Continue the daemon loop.
    /// \retval false Shut down the CLI daemon.
    bool run_once() override;

    /// \brief Called when the daemon thread starts.
    ///
    /// Installs line-editor callbacks and prints the greeting banner.
    void on_daemon_start() override;

    /// \brief Called when the daemon thread stops.
    ///
    /// Saves history and performs cleanup.
    void on_daemon_stop() override;

    /// \brief CLI is always a system actor.
    bool is_system_actor() const override {
        return true;
    }

    // --- Accessors for command handlers ---

    /// \brief Reference to the actor system.
    ActorSystem& system() {
        return system_;
    }

    /// \brief Read-only reference to the CLI configuration.
    const CliConfig& config() const {
        return config_;
    }

    /// \brief The current output formatter.
    ///
    /// \return Non-owning pointer. Never nullptr after construction.
    OutputFormatter* formatter() {
        return formatter_.get();
    }

    /// \brief The interactive pager for multi-page output.
    ///
    /// \return Non-owning pointer. Never nullptr after construction.
    Pager* pager() {
        return pager_.get();
    }

    /// \brief Whether the CLI input loop is still running.
    ///
    /// Set to false by /quit or EOF on stdin.
    ///
    /// \return true if the CLI is accepting input.
    bool is_running() const {
        return running_;
    }

    /// \brief Read-only access to the command tree.
    ///
    /// Exposed so that commands (e.g. /help) can walk the tree to
    /// generate help text or inspect available sub-commands.
    ///
    /// \return Non-owning pointer to the root \c CommandNode. Never
    ///         \c nullptr after construction.
    const CommandNode* command_tree() const {
        return command_tree_.get();
    }

    /// \brief Request the CLI input loop to exit.
    ///
    /// Sets \c running_ to \c false. The current command completes,
    /// then \c run_once() returns \c false and the daemon loop shuts
    /// down cleanly via \c on_daemon_stop().
    ///
    /// \note Callable from any command handler (CLI daemon thread).
    void request_shutdown() {
        running_ = false;
    }

    // --- ICliCommandHost interface ---

    /// \brief Send an InspectStateRequest to a target actor and block on
    ///        the reply.
    ///
    /// Polls this actor's mailbox on the dedicated thread. Safe because
    /// CliActor uses DispatchPolicy::DedicatedThread — no scheduler
    /// contention.
    ///
    /// \param[in] target Actor to inspect.
    /// \param[in] req The inspect request.
    /// \param[in] timeout Maximum time to wait for a reply.
    /// \return The reply if received within the timeout, otherwise
    ///         \c std::nullopt.
    std::optional<InspectStateReply>
    inspect(ActorId target, const InspectStateRequest& req,
            std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) override;

    /// \brief Send a KillRequest to a target actor and block on the reply.
    ///
    /// \param[in] target Actor to kill.
    /// \param[in] req The kill request.
    /// \param[in] timeout Maximum time to wait for a reply.
    /// \return The reply if received within the timeout, otherwise
    ///         \c std::nullopt.
    std::optional<KillReply>
    kill(ActorId target, const KillRequest& req,
         std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) override;

    /// \brief Quarantine or unquarantine an actor and block on the reply.
    ///
    /// \param[in] target Actor to quarantine/unquarantine.
    /// \param[in] req The quarantine request.
    /// \param[in] timeout Maximum time to wait for a reply.
    /// \return The reply if received within the timeout, otherwise
    ///         \c std::nullopt.
    std::optional<QuarantineReply>
    quarantine(ActorId target, const QuarantineRequest& req,
               std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) override;

    /// \brief Enumerate all known actors.
    ///
    /// \param[in] filter Optional substring filter on actor type or behavior
    ///                   name. Empty string matches all actors.
    /// \return Metadata for each matching actor.
    std::vector<ActorMeta> enumerate(std::string_view filter = "") override;

    // --- ISystemCliHost interface ---

    void render_system_stats(OutputFormatter& output) override;
    void render_memory_stats(OutputFormatter& output) override;
    void render_fault_status(OutputFormatter& output) override;
    void render_dlq_list(OutputFormatter& output,
                         std::string_view filter = "") override;
    result<void> dlq_replay(uint32_t index, ActorId target) override;

    // --- ILifecycleCliHost interface ---

    result<void> drain() override;
    result<void> shutdown() override;

    /// \brief Resolve the CLI history file path from config.
    ///
    /// If config.history_path is non-empty, returns it directly.
    /// Otherwise returns \c $HOME/.hpactor_history, falling back to
    /// \c /tmp/.hpactor_history if the home directory is unavailable.
    ///
    /// \param[in] config CLI configuration.
    /// \return The resolved history file path.
    static std::string get_history_path(const CliConfig& config);

    /// \brief Build an InspectStateReply for the CliActor itself without
    ///        going through the mailbox (avoids self-deadlock).
    ///
    /// Called from \c inspect() when the target is the CliActor's own
    /// actor ID.
    ///
    /// \param[in] req The inspect request (controls which sections to include).
    /// \return A fully populated \c InspectStateReply.
    InspectStateReply
    build_self_inspect_reply(const class InspectStateRequest& req);

  private:
    void build_command_tree();
    void print_greeting();

    /// \brief Poll mailbox for a message with the given TypeTag.
    ///
    /// Ignores all other messages until the expected tag arrives or the
    /// timeout expires.
    ///
    /// \param[in] expected_tag The TypeTag to wait for.
    /// \param[in] timeout Maximum time to poll.
    /// \return The raw payload if the expected message arrived, otherwise
    ///         \c std::nullopt.
    std::optional<StreamBuffer>
    poll_for_response(TypeTag expected_tag, std::chrono::milliseconds timeout);

    ActorSystem& system_;
    CliConfig config_;
    LineEditor line_editor_;
    std::unique_ptr<CommandNode> command_tree_;
    std::unique_ptr<OutputFormatter> formatter_;
    std::unique_ptr<Pager> pager_;
    std::unique_ptr<CliSession> session_;
    bool running_ = true;
};

} // namespace cli
} // namespace hpactor
