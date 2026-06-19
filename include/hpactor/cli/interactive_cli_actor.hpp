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
#include <hpactor/cli/line_editor.hpp>

#include <memory>
#include <string>

namespace hpactor {

class ActorSystem;

namespace cli {

class CliSession;
struct CommandNode;

/// \brief Base class for interactive CLI actors using the Template Method
///        pattern.
///
/// Extends \c DaemonActor and implements the three CLI host interfaces
/// (\c ICliCommandHost, \c ISystemCliHost, \c ILifecycleCliHost) but leaves
/// their methods pure virtual — subclasses provide transport-specific
/// implementations (local actor messaging vs. remote wire protocol).
///
/// Provides \c final Template Methods for the \c DaemonActor lifecycle
/// (\c on_daemon_start(), \c run_once(), \c on_daemon_stop()) with
/// \c protected virtual hooks for subclass customization of greetings,
/// configuration accessors, and transport-specific setup/teardown.
class InteractiveCliActor : public DaemonActor,
                            public ICliCommandHost,
                            public ISystemCliHost,
                            public ILifecycleCliHost {
  public:
    InteractiveCliActor(ActorContext* ctx, ActorSystem& system);
    ~InteractiveCliActor() override;

    // DaemonActor — Template Methods (final)
    bool run_once() final;
    void on_daemon_start() final;
    void on_daemon_stop() final;
    bool is_system_actor() const override {
        return true;
    }

    // Accessors
    ActorSystem& system() {
        return system_;
    }
    const CommandNode* command_tree() const {
        return command_tree_.get();
    }
    bool is_running() const {
        return running_;
    }
    void request_shutdown() {
        running_ = false;
    }

  protected:
    // ── Subclass hooks ────────────────────────────────────────────────

    /// \brief Print the welcome banner at session start.
    virtual void print_greeting() = 0;

    /// \brief Print the farewell message at session end.
    virtual void print_farewell() = 0;

    /// \brief Resolve the history file path for line editing.
    virtual std::string get_history_path() = 0;

    /// \brief Maximum number of in-memory history entries.
    virtual uint32_t get_history_max() = 0;

    /// \brief Default output format name ("pretty", "json", "tabular").
    virtual std::string get_default_format() = 0;

    /// \brief Number of items per paged output page.
    virtual uint32_t get_page_size() = 0;

    /// \brief Prompt string displayed to the user.
    virtual const char* get_prompt() {
        return "hpactor> ";
    }

    /// \brief Transport-specific hook called before each input line.
    ///
    /// \retval true  Continue the read-process loop.
    /// \retval false Shut down the daemon loop (e.g. exec-mode exit).
    virtual bool pre_input_hook() {
        return true;
    }

    /// \brief Transport-specific cleanup hook called before
    ///        \c on_daemon_stop() saves history and prints farewell.
    virtual void pre_stop_hook() {}

    /// \brief Hook called after the CliSession is created and the three
    ///        host interfaces (command_host, system_host, lifecycle_host)
    ///        are wired.  Subclasses override to wire their specific type
    ///        pointer (e.g. \c set_cli_actor, \c set_client_actor) into
    ///        the session so that \c CommandContext fields are populated.
    ///
    /// \param[in,out] session The newly-created CliSession.
    virtual void on_session_wired(CliSession& /*session*/) {}

    // ── Shared state ──────────────────────────────────────────────────

    ActorSystem& system_;
    std::unique_ptr<CommandNode> command_tree_;
    std::unique_ptr<LineEditor> line_editor_;
    std::unique_ptr<CliSession> session_;
    bool running_ = true;
};

} // namespace cli
} // namespace hpactor
