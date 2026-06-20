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
#include <hpactor/cli/cli_types.hpp>
#include <hpactor/cli/command_registry.hpp>
#include <hpactor/cli/output_formatter.hpp>
#include <hpactor/metrics/metrics_actor.hpp>

#include <cstdio>
#include <string>

namespace hpactor {
namespace cli {
namespace {

class MetricsShowCommand final : public ICommand {
  public:
    std::string_view path() const noexcept override {
        return "metrics/show";
    }
    std::string_view help_text() const noexcept override {
        return "Show current metrics snapshot";
    }
    int order() const noexcept override {
        return 100;
    }

    result<void> execute(CommandContext& ctx) const override {
        // Remote CLI: delegate to system_host which sends to the server.
        if (ctx.system_host &&
            ctx.system_host->execute_path("metrics/show", {}, {}, *ctx.output))
            return result<void>::make();
        // Local CLI: access metrics_actor directly.
        ctx.output->header("Metrics");
        auto* sys = ctx.system;
        if (!sys) {
            ctx.output->error("Internal error: no actor system");
            return result<void>::make();
        }
        auto* metrics_actor = sys->metrics_actor();
        if (!metrics_actor) {
            ctx.output->raw("Metrics subsystem is not enabled.");
            return result<void>::make();
        }
        std::string snapshot = metrics_actor->format_snapshot();
        ctx.output->raw(snapshot);
        return result<void>::make();
    }
};

class TopologyShowCommand final : public ICommand {
  public:
    std::string_view path() const noexcept override {
        return "topology/show";
    }
    std::string_view help_text() const noexcept override {
        return "Show topology tree";
    }
    int order() const noexcept override {
        return 100;
    }

    result<void> execute(CommandContext& ctx) const override {
        ctx.output->header("Topology");
        auto* sys = ctx.system;
        if (!sys) {
            ctx.output->error("Internal error: no actor system");
            return result<void>::make();
        }

        std::string tree;
        tree += "ActorSystem";
        auto* sched = sys->scheduler();
        if (sched) {
            tree += " (" + std::to_string(sched->worker_count()) +
                    " scheduler threads, A2WS)";
        }
        tree += "\n";

        sys->for_each_actor([&](ActorId /*id*/, AbstractActor& actor) {
            auto meta = actor.to_metadata();
            char line[256];
            const char* marker = actor.is_system_actor() ? " [system]" : "";
            int n = snprintf(line, sizeof(line), "  %s%s\n",
                             meta.actor_type.c_str(), marker);
            tree.append(line, static_cast<size_t>(n));
        });

        ctx.output->raw(tree);
        return result<void>::make();
    }
};

const CommandRegistration<MetricsShowCommand> kRegisterMetricsShow;
const CommandRegistration<TopologyShowCommand> kRegisterTopologyShow;

} // anonymous namespace
} // namespace cli
} // namespace hpactor