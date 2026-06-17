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

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace hpactor {

class ActorSystem;

namespace cli {

class CliActor;
struct CommandNode;
class OutputFormatter;
class Pager;
struct Token;

/// \brief Transport-agnostic CLI command processor.
///
/// Walks a command tree, tokenizes input, dispatches to registered
/// command handlers, and routes formatted output through a callback.
/// Designed to be reused across stdin-based (CliActor) and
/// socket-based (CliLegacyServerActor) transports.
///
/// \note Thread affinity: all methods are called from the owning
///       daemon/transport thread.
class CliSession {
  public:
    /// \brief Construct a CLI session.
    ///
    /// \param[in] system Actor system (may be nullptr for unit tests).
    /// \param[in] command_tree Root of the command trie. Must outlive
    ///                         the session.
    /// \param[in] formatter Output formatter for rendering results.
    /// \param[in] output_fn Callback invoked with the final formatted
    ///                      output string (including trailing newline).
    /// \param[in] page_size Number of items per paged output page.
    CliSession(ActorSystem* system, const CommandNode* command_tree,
               std::unique_ptr<OutputFormatter> formatter,
               std::function<void(const std::string&)> output_fn,
               uint32_t page_size = 50);

    ~CliSession();

    /// \brief Process a single command line.
    ///
    /// Tokenizes, walks the command tree, and dispatches to the
    /// matched handler. Output is written via the \c output_fn
    /// callback.
    ///
    /// \param[in] line Raw input line (may include leading "/").
    /// \retval true  Continue accepting input.
    /// \retval false Session has been shut down.
    bool process_line(const std::string& line);

    /// \brief Request the session to shut down.
    ///
    /// Causes the next \c process_line() call to return false.
    void request_shutdown();

    /// \brief Return the command tree used by this session.
    ///
    /// Allows commands (like /help) to render the full command tree
    /// even when neither cli_actor nor cli_server_actor is set (e.g.,
    /// in CliClientActor's local session).
    const struct CommandNode* get_command_tree() const {
        return command_tree_;
    }

    /// \brief Set the owning CliActor, if any.
    ///
    /// Command handlers that require the CliActor interface
    /// (request-reply, enumerate_actors, etc.) operate through
    /// \c ctx.cli_actor, which is populated from this pointer.
    ///
    /// \param[in] actor Pointer to the owning CliActor (may be null).
    void set_cli_actor(class CliActor* actor) {
        cli_actor_ = actor;
    }

    /// \brief Set the owning CliLegacyServerActor, if any.
    ///
    /// Command handlers that require request-reply or enumeration
    /// fall back to \c ctx.cli_server_actor when \c ctx.cli_actor is null.
    ///
    /// \param[in] server Pointer to the owning CliLegacyServerActor (may be
    /// null).
    void set_cli_server_actor(class CliLegacyServerActor* server) {
        cli_server_actor_ = server;
    }

    /// \brief Set the command host for actor operations.
    void set_command_host(class ICliCommandHost* host) {
        command_host_ = host;
    }

    /// \brief Set the system host for system queries.
    void set_system_host(class ISystemCliHost* host) {
        system_host_ = host;
    }

    /// \brief Set the lifecycle host for drain/shutdown.
    void set_lifecycle_host(class ILifecycleCliHost* host) {
        lifecycle_host_ = host;
    }

    /// \brief Access the pager for multi-page output.
    /// \return Non-owning pointer. Never null after construction.
    Pager* pager() {
        return pager_.get();
    }

    /// \brief Access the current output formatter.
    /// \return Non-owning pointer. Never null after construction.
    OutputFormatter* formatter() {
        return formatter_.get();
    }

  private:
    void execute_tokens(const std::vector<Token>& tokens);

    ActorSystem* system_;
    const CommandNode* command_tree_;
    std::unique_ptr<OutputFormatter> formatter_;
    std::unique_ptr<Pager> pager_;
    std::function<void(const std::string&)> output_fn_;
    uint32_t page_size_;
    bool keep_running_ = true;
    std::string current_format_ = "pretty";
    class CliActor* cli_actor_ = nullptr;
    class CliLegacyServerActor* cli_server_actor_ = nullptr;
    class ICliCommandHost* command_host_ = nullptr;
    class ISystemCliHost* system_host_ = nullptr;
    class ILifecycleCliHost* lifecycle_host_ = nullptr;
};

} // namespace cli
} // namespace hpactor
