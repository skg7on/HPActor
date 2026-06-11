// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#include <hpactor/cli/command_registry.hpp>
#include <hpactor/cli/output_formatter.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/sched/scheduler.hpp>

#include <string>
#include <vector>

namespace hpactor {
namespace cli {
namespace {

class SchedulerWorkersCommand final : public ICommand {
  public:
    std::string_view path() const noexcept override {
        return "scheduler/workers";
    }
    std::string_view help_text() const noexcept override {
        return "Show per-worker thread statistics";
    }
    int order() const noexcept override {
        return 720;
    }

    result<void> execute(CommandContext& ctx) const override {
        auto* sys = ctx.system;
        if (!sys) {
            ctx.output->error("Internal error: no actor system");
            return result<void>::make();
        }
        auto* sched = sys->scheduler();
        if (!sched) {
            ctx.output->raw("Scheduler is not running.");
            return result<void>::make();
        }

        auto snaps = sched->worker_snapshots();
        ctx.output->header("Scheduler Workers (" +
                           std::to_string(sched->worker_count()) +
                           " threads, A2WS)");

        if (snaps.empty()) {
            ctx.output->raw("Per-worker statistics not available.");
            return result<void>::make();
        }

        std::vector<std::string> cols = {"Worker", "Steal Donations", "Idle"};
        std::vector<std::vector<std::string>> rows;
        for (auto& ws : snaps) {
            rows.push_back({
                std::to_string(ws.worker_index),
                std::to_string(ws.steals_attempted),
                ws.is_idle ? "yes" : "no",
            });
        }
        ctx.output->table(cols, rows);
        return result<void>::make();
    }
};

const CommandRegistration<SchedulerWorkersCommand> kRegisterSchedulerWorkers;

} // anonymous namespace
} // namespace cli
} // namespace hpactor
