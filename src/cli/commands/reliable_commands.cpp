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

#include <hpactor/actor/actor_system.hpp>
#include <hpactor/cli/command/command_registry.hpp>
#include <hpactor/cli/format/output_formatter.hpp>
#include <hpactor/msg/outbound_delivery_tracker.hpp>
#include <hpactor/types/types.hpp>

#include <charconv>
#include <sstream>
#include <string>

namespace hpactor {
namespace cli {
namespace {

msg::OutboundDeliveryTracker* resolve_tracker(CommandContext& ctx) {
    auto* system = ctx.system;
    if (!system) {
        ctx.output->error("No actor system available");
        return nullptr;
    }
    auto* tracker = system->outbound_tracker();
    if (!tracker) {
        ctx.output->raw("Reliable delivery tracker is not enabled.");
    }
    return tracker;
}

// ── /reliable/outbox ────────────────────────────────────────────────────

class ReliableOutboxCommand final : public ICommand {
  public:
    std::string_view path() const noexcept override {
        return "reliable/outbox";
    }
    std::string_view help_text() const noexcept override {
        return "List pending at-least-once sends";
    }
    int order() const noexcept override {
        return 510;
    }

    result<void> execute(CommandContext& ctx) const override {
        auto* tracker = resolve_tracker(ctx);
        if (!tracker)
            return result<void>::make();

        auto pending = tracker->snapshot();
        if (pending.empty()) {
            ctx.output->raw("No pending sends.");
            return result<void>::make();
        }

        std::stringstream ss;
        ss << "msg_id  attempt  next_retry_ns\n";
        ss << "------  -------  -------------\n";
        for (auto& ps : pending) {
            ss << ps.msg_id.value() << "  " << static_cast<int>(ps.retry_count)
               << "  " << ps.next_retry_ns << "\n";
        }
        ctx.output->raw(ss.str());
        return result<void>::make();
    }
};

const CommandRegistration<ReliableOutboxCommand> kRegisterReliableOutbox;

// ── /reliable/outbox/<msg_id> ───────────────────────────────────────────

class ReliableOutboxShowCommand final : public ICommand {
  public:
    std::string_view path() const noexcept override {
        return "reliable/outbox/<msg_id>";
    }
    std::string_view help_text() const noexcept override {
        return "Show details for one pending send";
    }
    int order() const noexcept override {
        return 511;
    }

    result<void> execute(CommandContext& ctx) const override {
        auto* tracker = resolve_tracker(ctx);
        if (!tracker)
            return result<void>::make();

        auto it = ctx.params.find("<msg_id>");
        if (it == ctx.params.end()) {
            ctx.output->error("Missing <msg_id> parameter");
            return result<void>::make();
        }

        bool ok = false;
        uint64_t msg_id_val = 0;
        auto [ptr, ec] = std::from_chars(
            it->second.data(), it->second.data() + it->second.size(), msg_id_val);
        ok = (ec == std::errc{});

        if (!ok) {
            ctx.output->error("Invalid msg_id: " + it->second);
            return result<void>::make();
        }

        auto pending = tracker->snapshot();
        for (auto& ps : pending) {
            if (ps.msg_id.value() == msg_id_val) {
                std::stringstream ss;
                ss << "msg_id:       " << ps.msg_id.value() << "\n"
                   << "retry_count:  " << static_cast<int>(ps.retry_count) << "\n"
                   << "deadline_ns:  " << ps.deadline_ns << "\n"
                   << "next_retry:   " << ps.next_retry_ns << "\n";
                ctx.output->raw(ss.str());
                return result<void>::make();
            }
        }
        ctx.output->raw("No pending send with that msg_id.");
        return result<void>::make();
    }
};

const CommandRegistration<ReliableOutboxShowCommand> kRegisterReliableOutboxShow;

// ── /reliable/cancel/<msg_id> ───────────────────────────────────────────

class ReliableCancelCommand final : public ICommand {
  public:
    std::string_view path() const noexcept override {
        return "reliable/cancel/<msg_id>";
    }
    std::string_view help_text() const noexcept override {
        return "Cancel tracking for a pending send";
    }
    int order() const noexcept override {
        return 512;
    }

    result<void> execute(CommandContext& ctx) const override {
        auto* tracker = resolve_tracker(ctx);
        if (!tracker)
            return result<void>::make();

        auto it = ctx.params.find("<msg_id>");
        if (it == ctx.params.end()) {
            ctx.output->error("Missing <msg_id> parameter");
            return result<void>::make();
        }

        bool ok = false;
        uint64_t msg_id_val = 0;
        auto [ptr, ec] = std::from_chars(
            it->second.data(), it->second.data() + it->second.size(), msg_id_val);
        ok = (ec == std::errc{});

        if (!ok) {
            ctx.output->error("Invalid msg_id: " + it->second);
            return result<void>::make();
        }

        tracker->cancel(MessageId{msg_id_val});
        ctx.output->raw("Cancelled.");
        return result<void>::make();
    }
};

const CommandRegistration<ReliableCancelCommand> kRegisterReliableCancel;

// ── /reliable/stats ─────────────────────────────────────────────────────

class ReliableStatsCommand final : public ICommand {
  public:
    std::string_view path() const noexcept override {
        return "reliable/stats";
    }
    std::string_view help_text() const noexcept override {
        return "Show reliable delivery counters";
    }
    int order() const noexcept override {
        return 513;
    }

    result<void> execute(CommandContext& ctx) const override {
        auto* tracker = resolve_tracker(ctx);
        if (!tracker)
            return result<void>::make();

        std::stringstream ss;
        ss << "pending: " << tracker->pending() << "\n";
        auto pending = tracker->snapshot();
        ss << "snapshot_entries: " << pending.size() << "\n";
        ctx.output->raw(ss.str());
        return result<void>::make();
    }
};

const CommandRegistration<ReliableStatsCommand> kRegisterReliableStats;

} // namespace
} // namespace cli
} // namespace hpactor
