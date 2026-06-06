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

#include "ask_commands.hpp"

#include <hpactor/cli/command_context.hpp>
#include <hpactor/cli/command_node.hpp>
#include <hpactor/cli/command_registry.hpp>
#include <hpactor/cli/output_formatter.hpp>

#include <string>
#include <string_view>

namespace hpactor {
namespace cli {
namespace {

// ---------------------------------------------------------------------------
// /ask pending — list in-flight ask requests
// ---------------------------------------------------------------------------
class AskPendingCommand final : public ICommand {
  public:
    std::string_view path() const noexcept override {
        return "ask/pending";
    }
    std::string_view help_text() const noexcept override {
        return "List in-flight ask requests";
    }
    int order() const noexcept override {
        return 600;
    }

    result<void> execute(CommandContext& ctx) const override {
        ctx.output->raw("ask pending: not yet implemented");
        return result<void>::make();
    }
};

// ---------------------------------------------------------------------------
// /ask cancel <msg_id> — cancel an in-flight ask request by message ID
// ---------------------------------------------------------------------------
class AskCancelCommand final : public ICommand {
  public:
    std::string_view path() const noexcept override {
        return "ask/cancel";
    }
    std::string_view help_text() const noexcept override {
        return "Cancel an in-flight ask request by message ID: /ask cancel "
               "--msg-id N";
    }
    int order() const noexcept override {
        return 610;
    }

    result<void> execute(CommandContext& ctx) const override {
        auto msg_id = ctx.get_param("msg-id");
        if (!msg_id) {
            ctx.output->error("Usage: /ask cancel --msg-id N");
            return result<void>::make();
        }
        ctx.output->raw("ask cancel " + *msg_id + ": not yet implemented");
        return result<void>::make();
    }
};

// ---------------------------------------------------------------------------
// /ask stats — show ask manager statistics
// ---------------------------------------------------------------------------
class AskStatsCommand final : public ICommand {
  public:
    std::string_view path() const noexcept override {
        return "ask/stats";
    }
    std::string_view help_text() const noexcept override {
        return "Show ask manager statistics";
    }
    int order() const noexcept override {
        return 620;
    }

    result<void> execute(CommandContext& ctx) const override {
        ctx.output->raw("ask stats: not yet implemented");
        return result<void>::make();
    }
};

// ── Auto-registration via file-scope objects ────────────────────────────

const CommandRegistration<AskPendingCommand> kRegisterAskPending;
const CommandRegistration<AskCancelCommand> kRegisterAskCancel;
const CommandRegistration<AskStatsCommand> kRegisterAskStats;

} // anonymous namespace

void register_ask_commands(CommandNode& /*root*/) {
    // Commands are auto-registered via file-scope CommandRegistration<T>
    // objects above. This function is a forward hook for the call site in
    // CliActor::build_command_tree() — reserved for manual command mounting
    // when the AskManager inspection API is exposed.
}

} // namespace cli
} // namespace hpactor
