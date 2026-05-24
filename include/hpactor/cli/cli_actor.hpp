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

// Forward-declare protobuf types (defined in cli_messages.pb.h)
class InspectStateReply;
class KillReply;
class ListActorsReply;
class SystemStatsReply;
class MemoryStatsReply;

class CliActor : public DaemonActor {
  public:
    CliActor(ActorContext* ctx, ActorSystem& system, const CliConfig& config);

    // DaemonActor interface
    bool run_once() override;
    void on_daemon_start() override;
    void on_daemon_stop() override;

    bool is_system_actor() const override {
        return true;
    }

    // Accessors for commands
    ActorSystem& system() {
        return system_;
    }
    const CliConfig& config() const {
        return config_;
    }
    OutputFormatter* formatter() {
        return formatter_.get();
    }
    Pager* pager() {
        return pager_.get();
    }

    // Whether the CLI input loop is still running.
    // Set to false by /quit or EOF on stdin.
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

    // --- Request-Response Helpers ---
    //
    // Send an InspectStateRequest to target and block on the reply.
    // Polls this actor's mailbox on the dedicated thread — safe, no
    // scheduler contention since CliActor uses DispatchPolicy::DedicatedThread.
    std::optional<InspectStateReply> send_and_wait_inspect(
        ActorId target, const class InspectStateRequest& req,
        std::chrono::milliseconds timeout = std::chrono::milliseconds(2000));

    std::optional<KillReply> send_and_wait_kill(
        ActorId target, const class KillRequest& req,
        std::chrono::milliseconds timeout = std::chrono::milliseconds(2000));

    // Enumerate all known actors. Returns metadata for each.
    std::vector<ActorMeta> enumerate_actors(const std::string& filter = "");

    // Resolve the CLI history file path from config.
    // If config.history_path is non-empty, returns it directly.
    // Otherwise returns $HOME/.hpactor_history, falling back to
    // /tmp/.hpactor_history.
    static std::string get_history_path(const CliConfig& config);

  private:
    void build_command_tree();
    void execute_tokens(const std::vector<Token>& tokens);
    void print_greeting();

    // Poll mailbox for a message with the given TypeTag, ignoring all others.
    // Returns the raw StreamBuffer payload if found before timeout.
    std::optional<StreamBuffer>
    poll_for_response(TypeTag expected_tag, std::chrono::milliseconds timeout);

    ActorSystem& system_;
    CliConfig config_;
    LineEditor line_editor_;
    std::unique_ptr<CommandNode> command_tree_;
    std::unique_ptr<OutputFormatter> formatter_;
    std::unique_ptr<Pager> pager_;
    bool running_ = true;
};

} // namespace cli
} // namespace hpactor