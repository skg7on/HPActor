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

#include <hpactor/cli/cli_session.hpp>
#include <hpactor/cli/command_context.hpp>
#include <hpactor/cli/command_node.hpp>
#include <hpactor/cli/command_registry.hpp>
#include <hpactor/cli/command_tree_builder.hpp>
#include <hpactor/cli/interactive_cli_actor.hpp>
#include <hpactor/cli/line_editor.hpp>
#include <hpactor/cli/output_formatter.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/fault/fault_macros.hpp>

#include <cstdio>

namespace hpactor {
namespace cli {

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

InteractiveCliActor::InteractiveCliActor(ActorContext* ctx, ActorSystem& system)
    : DaemonActor(ctx, system), system_(system) {}

InteractiveCliActor::~InteractiveCliActor() = default;

// ---------------------------------------------------------------------------
// Template Methods (final) — DaemonActor lifecycle
// ---------------------------------------------------------------------------

void InteractiveCliActor::on_daemon_start() {
    // 1. Build command tree from global CommandRegistry.
    command_tree_ = std::make_unique<CommandNode>("/", "CLI root");
    build_command_tree_from_registry(*command_tree_);

    // 2. Create line editor (history config from virtual hooks).
    LineEditorConfig editor_cfg;
    editor_cfg.history_path = get_history_path();
    editor_cfg.history_max = get_history_max();
    editor_cfg.multiline = false;
    line_editor_ = std::make_unique<LineEditor>(editor_cfg, command_tree_.get());
    line_editor_->load_history();

    // 3. Create session (format + page size from virtual hooks).
    session_ = std::make_unique<CliSession>(
        &system_, command_tree_.get(),
        OutputFormatter::create(get_default_format()),
        [](const std::string& text) { std::printf("%s", text.c_str()); },
        get_page_size());

    // 4. Wire host interfaces — this object IS all three hosts.
    session_->set_command_host(this);
    session_->set_system_host(this);
    session_->set_lifecycle_host(this);

    // 5. Subclass hook — print the welcome banner.
    print_greeting();
}

bool InteractiveCliActor::run_once() {
    FAULT_INJECT("hpactor.cli.actor.run_once.fail") {
        return false; // daemon exits
    }
    if (!running_)
        return false;

    // Transport-specific preprocessing (connection retry, exec mode, etc.).
    if (!pre_input_hook())
        return false;

    std::string line = line_editor_->readline(get_prompt());
    if (line.empty()) {
        if (std::feof(stdin)) {
            print_farewell();
            running_ = false;
            return false;
        }
        return true; // empty input (just Enter) — keep running
    }

    bool keep_going = session_->process_line(line);
    line_editor_->add_history(line);
    if (!keep_going) {
        running_ = false;
        return false;
    }
    return true;
}

void InteractiveCliActor::on_daemon_stop() {
    pre_stop_hook();
    if (line_editor_)
        line_editor_->save_history();
    print_farewell();
}

} // namespace cli
} // namespace hpactor
