// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#include <hpactor/actor/actor_system.hpp>
#include <hpactor/cli/command_registry.hpp>
#include <hpactor/cli/output_formatter.hpp>

#include "command_utils.hpp"

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
        return "Show per-worker thread statistics and idle model (polling / CV)";
    }
    int order() const noexcept override {
        return 720;
    }

    result<void> execute(CommandContext& ctx) const override {
        // Remote CLI: delegate to system_host which sends to the server.
        if (ctx.system_host && ctx.system_host->execute_path("scheduler/workers",
                                                             {}, {}, *ctx.output))
            return result<void>::make();
        // Local CLI: render from the local ActorSystem.
        auto* sys = ctx.system;
        if (!sys) {
            ctx.output->error("Internal error: no actor system");
            return result<void>::make();
        }
        render_scheduler_workers(*sys, *ctx.output);
        return result<void>::make();
    }
};

const CommandRegistration<SchedulerWorkersCommand> kRegisterSchedulerWorkers;

} // anonymous namespace
} // namespace cli
} // namespace hpactor
