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
#include <hpactor/core/actor_system.hpp>

#include <charconv>
#include <map>
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
        auto* sys = ctx.system;
        if (!sys || !sys->ask_manager()) {
            ctx.output->raw("Ask subsystem is not available.");
            return result<void>::make();
        }
        auto* am = sys->ask_manager();
        auto pending = am->snapshot();

        ctx.output->header("In-Flight Ask Requests (" +
                           std::to_string(pending.size()) + " pending)");

        if (pending.empty()) {
            ctx.output->raw("No pending asks.");
            return result<void>::make();
        }

        std::vector<std::string> cols = {"MsgID", "Requester", "Elapsed"};
        std::vector<std::vector<std::string>> rows;
        for (auto& e : pending) {
            char id_buf[32], req_buf[32], elapsed_buf[32];
            snprintf(id_buf, sizeof(id_buf), "0x%04llX",
                     static_cast<unsigned long long>(e.msg_id));
            snprintf(req_buf, sizeof(req_buf), "Actor-0x%04llX",
                     static_cast<unsigned long long>(e.requester_id));
            snprintf(elapsed_buf, sizeof(elapsed_buf), "%llums",
                     static_cast<unsigned long long>(e.elapsed_ms));
            rows.push_back({id_buf, req_buf, elapsed_buf});
        }
        ctx.output->table(cols, rows);
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
        auto msg_id_str = ctx.get_param("msg-id");
        if (!msg_id_str) {
            ctx.output->error("Usage: /ask cancel --msg-id N");
            return result<void>::make();
        }
        auto* sys = ctx.system;
        if (!sys || !sys->ask_manager()) {
            ctx.output->raw("Ask subsystem is not available.");
            return result<void>::make();
        }
        bool ok = false;
        uint64_t msg_id_val = 0;
        auto [ptr, ec] =
            std::from_chars(msg_id_str->data(),
                            msg_id_str->data() + msg_id_str->size(), msg_id_val);
        ok = (ec == std::errc{});
        if (!ok) {
            ctx.output->error("Invalid msg-id: " + *msg_id_str);
            return result<void>::make();
        }
        bool cancelled = sys->ask_manager()->cancel(msg_id_val);
        if (cancelled) {
            ctx.output->raw("Ask " + *msg_id_str + " cancelled.");
        } else {
            ctx.output->raw("Ask " + *msg_id_str +
                            " not found (already resolved or never registered).");
        }
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
        auto* sys = ctx.system;
        if (!sys || !sys->ask_manager()) {
            ctx.output->raw("Ask subsystem is not available.");
            return result<void>::make();
        }
        auto s = sys->ask_manager()->stats();

        ctx.output->header("Ask Manager Statistics");
        std::map<std::string, std::string> kv;
        kv["Total registered"] = std::to_string(s.total_registered);
        kv["Total resolved"] = std::to_string(s.total_resolved);
        kv["Total timed out"] = std::to_string(s.total_timed_out);
        kv["Total cancelled"] = std::to_string(s.total_cancelled);
        kv["Currently pending"] = std::to_string(s.pending);
        ctx.output->key_value(kv);
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
