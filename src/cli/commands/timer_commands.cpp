// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#include <hpactor/actor/actor_system.hpp>
#include <hpactor/cli/command/command_registry.hpp>
#include <hpactor/cli/format/output_formatter.hpp>
#include <hpactor/sched/scheduler_interfaces.hpp>
#include <hpactor/timer/timer_stats_snapshot.hpp>

#include <charconv>
#include <cinttypes>
#include <cstdio>
#include <string>
#include <vector>

namespace hpactor {
namespace cli {
namespace {

// ── Helper: parse a hex or decimal integer from a string ──────────────────

uint64_t parse_uint64(const std::string& s) {
    uint64_t raw = 0;
    int base = 10;
    const char* start = s.data();
    if (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        start = s.data() + 2;
    }
    auto [ptr, ec] = std::from_chars(start, s.data() + s.size(), raw, base);
    if (ec != std::errc{})
        return 0;
    return raw;
}

// ── Helper: format a nanosecond deadline as a human-readable string ────────

std::string format_deadline(int64_t deadline_ns) {
    if (deadline_ns == INT64_MAX)
        return "none";
    char buf[64];
    if (deadline_ns < 1'000'000LL) {
        snprintf(buf, sizeof(buf), "%" PRId64 " ns", deadline_ns);
    } else if (deadline_ns < 1'000'000'000LL) {
        snprintf(buf, sizeof(buf), "%.2f ms",
                 static_cast<double>(deadline_ns) / 1'000'000.0);
    } else {
        snprintf(buf, sizeof(buf), "%.2f s",
                 static_cast<double>(deadline_ns) / 1'000'000'000.0);
    }
    return buf;
}

// ── Helper: format a large uint64_t with separators (not using locales) ──
//    We just use snprintf because the standard formatters handle this fine.

// ═══════════════════════════════════════════════════════════════════════════
// /timer stats          — aggregate stats across all shards
// /timer stats <shard>  — per-shard detail
// ═══════════════════════════════════════════════════════════════════════════

class TimerStatsCommand final : public ICommand {
  public:
    std::string_view path() const noexcept override {
        return "timer/stats";
    }
    std::string_view help_text() const noexcept override {
        return "Show timer statistics.  Optional <shard_index> for per-shard detail.";
    }
    int order() const noexcept override {
        return 730;
    }

    result<void> execute(CommandContext& ctx) const override {
        auto* sys = ctx.system;
        if (!sys) {
            ctx.output->error("Internal error: no actor system");
            return result<void>::make();
        }

        auto snap = sys->timer_stats();

        // ── Per-shard detail when a shard index is provided ──
        if (!ctx.args.empty()) {
            uint64_t shard_idx = parse_uint64(ctx.args[0]);
            if (shard_idx >= snap.shards.size()) {
                ctx.output->error("Shard index " + ctx.args[0] +
                                  " out of range (num_shards=" +
                                  std::to_string(snap.shards.size()) + ")");
                return result<void>::make();
            }
            render_shard_detail(shard_idx, snap, *ctx.output);
            return result<void>::make();
        }

        // ── Aggregate view ──
        render_aggregate(snap, *ctx.output);
        return result<void>::make();
    }

  private:
    static void render_aggregate(const sched::TimerStatsSnapshot& snap,
                                 OutputFormatter& output) {
        output.header("Timer Statistics");

        char pending_buf[32];
        char scheduled_buf[32];
        char fired_buf[32];
        char cancelled_buf[32];
        char late_buf[32];
        char dropped_buf[32];

        snprintf(pending_buf, sizeof(pending_buf), "%" PRIu64, snap.total_pending);
        snprintf(scheduled_buf, sizeof(scheduled_buf), "%" PRIu64,
                 snap.total_scheduled);
        snprintf(fired_buf, sizeof(fired_buf), "%" PRIu64, snap.total_fired);
        snprintf(cancelled_buf, sizeof(cancelled_buf), "%" PRIu64,
                 snap.total_cancelled);
        snprintf(late_buf, sizeof(late_buf), "%" PRIu64, snap.total_late);
        snprintf(dropped_buf, sizeof(dropped_buf), "%" PRIu64, snap.total_dropped);

        std::map<std::string, std::string> kv;
        kv["Total shards"] = std::to_string(snap.num_shards);
        kv["Pending timers"] = pending_buf;
        kv["Scheduled total"] = scheduled_buf;
        kv["Fired total"] = fired_buf;
        kv["Cancelled total"] = cancelled_buf;
        kv["Late firings"] = late_buf;
        kv["Dropped"] = dropped_buf;
        kv["Next deadline"] = format_deadline(snap.next_deadline);

        output.key_value(kv);
    }

    static void render_shard_detail(uint64_t shard_idx,
                                    const sched::TimerStatsSnapshot& snap,
                                    OutputFormatter& output) {
        const auto& s = snap.shards[shard_idx];
        output.header("Timer Shard " + std::to_string(shard_idx));

        char pending_buf[32];
        char cmdq_buf[32];
        char fired_buf[32];
        char late_buf[32];
        char dropped_buf[32];

        snprintf(pending_buf, sizeof(pending_buf), "%" PRIu64, s.pending);
        snprintf(cmdq_buf, sizeof(cmdq_buf), "%" PRIu64, s.cmd_queue_depth);
        snprintf(fired_buf, sizeof(fired_buf), "%" PRIu64, s.fired);
        snprintf(late_buf, sizeof(late_buf), "%" PRIu64, s.late);
        snprintf(dropped_buf, sizeof(dropped_buf), "%" PRIu64, s.dropped);

        std::map<std::string, std::string> kv;
        kv["Pending"] = pending_buf;
        kv["Command queue depth"] = cmdq_buf;
        kv["Fired"] = fired_buf;
        kv["Late"] = late_buf;
        kv["Dropped"] = dropped_buf;
        kv["Min deadline"] = format_deadline(s.min_deadline);

        output.key_value(kv);
    }
};

const CommandRegistration<TimerStatsCommand> kRegisterTimerStats;

// ═══════════════════════════════════════════════════════════════════════════
// /timer inspect <handle> — resolve a TimerHandle
// ═══════════════════════════════════════════════════════════════════════════

class TimerInspectCommand final : public ICommand {
  public:
    std::string_view path() const noexcept override {
        return "timer/inspect";
    }
    std::string_view help_text() const noexcept override {
        return "Inspect a timer by handle (hex or decimal)";
    }
    int order() const noexcept override {
        return 731;
    }

    result<void> execute(CommandContext& ctx) const override {
        if (ctx.args.empty()) {
            ctx.output->error("Usage: /timer inspect <handle>");
            return result<void>::make();
        }

        uint64_t raw = parse_uint64(ctx.args[0]);
        if (raw == 0) {
            ctx.output->error("Invalid timer handle: " + ctx.args[0]);
            return result<void>::make();
        }

        auto handle = sched::TimerHandle{raw};
        uint32_t shard = sched::TimerHandle::shard_index(handle);
        uint32_t slot = sched::TimerHandle::slot_index(handle);
        uint8_t gen = sched::TimerHandle::generation(handle);
        uint16_t tag = sched::TimerHandle::type_tag(handle);

        // Check whether the shard index is within range.
        auto* sys = ctx.system;
        std::string status = "not found";
        if (sys) {
            auto snap = sys->timer_stats();
            if (shard < snap.shards.size()) {
                // We cannot fully determine status without deeper shard
                // introspection (which would require holding the shard mutex).
                // For now, report "unknown" when the handle's shard is valid.
                status = "unknown (valid shard)";
            } else {
                status = "not found (shard out of range)";
            }
        }

        char hex_buf[24];
        snprintf(hex_buf, sizeof(hex_buf), "0x%" PRIx64, raw);

        output_header(*ctx.output, "Timer Handle Inspection");
        std::map<std::string, std::string> kv;
        kv["Handle"] = hex_buf;
        kv["Shard"] = std::to_string(shard);
        kv["Slot"] = std::to_string(slot);
        kv["Generation"] = std::to_string(gen);
        kv["Type tag"] = std::to_string(tag);
        kv["Status"] = status;
        ctx.output->key_value(kv);

        return result<void>::make();
    }

  private:
    static void output_header(OutputFormatter& output, const std::string& title) {
        output.header(title);
    }
};

const CommandRegistration<TimerInspectCommand> kRegisterTimerInspect;

// ═══════════════════════════════════════════════════════════════════════════
// /timer groups — list timer groups (stub for V1)
// ═══════════════════════════════════════════════════════════════════════════

class TimerGroupsCommand final : public ICommand {
  public:
    std::string_view path() const noexcept override {
        return "timer/groups";
    }
    std::string_view help_text() const noexcept override {
        return "List timer groups (not yet tracked at system level)";
    }
    int order() const noexcept override {
        return 732;
    }

    result<void> execute(CommandContext& ctx) const override {
        ctx.output->header("Timer Groups");
        ctx.output->raw("Timer groups not yet tracked at system level");
        return result<void>::make();
    }
};

const CommandRegistration<TimerGroupsCommand> kRegisterTimerGroups;

} // anonymous namespace
} // namespace cli
} // namespace hpactor
