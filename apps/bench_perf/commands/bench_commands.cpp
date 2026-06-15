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

#include <hpactor/cli/cli_actor.hpp>
#include <hpactor/cli/command_registry.hpp>
#include <hpactor/cli/output_formatter.hpp>
#include <hpactor/cli_messages.pb.h>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/msg/typed_message.hpp>

#include <chrono>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace hpactor {
namespace cli {
namespace {

// =============================================================================
// Helper: find coordinator actor by type name
// =============================================================================

static ActorId find_coordinator(CliActor* cli) {
    auto actors = cli->enumerate("BenchCoordinatorActor");
    if (actors.empty())
        return ActorId{0};
    return ActorId{actors[0].actor_id};
}

// =============================================================================
// Helper: find collector actor by type name
// =============================================================================

static ActorId find_collector(CliActor* cli) {
    auto actors = cli->enumerate("BenchCollectorActor");
    if (actors.empty())
        return ActorId{0};
    return ActorId{actors[0].actor_id};
}

// =============================================================================
// Helper: parse key=value text state into a map
// =============================================================================

static std::map<std::string, std::string> parse_state(const std::string& state) {
    std::map<std::string, std::string> m;
    std::istringstream iss(state);
    std::string line;
    while (std::getline(iss, line)) {
        auto eq = line.find('=');
        if (eq != std::string::npos)
            m[line.substr(0, eq)] = line.substr(eq + 1);
    }
    return m;
}

// =============================================================================
// /bench start <preset> — Start a benchmark run
// =============================================================================

class BenchStartCommand final : public ICommand {
  public:
    std::string_view path() const noexcept override {
        return "bench/start";
    }
    std::string_view help_text() const noexcept override {
        return "Start a benchmark run. Usage: /bench start <preset>";
    }
    int order() const noexcept override {
        return 500;
    }

    result<void> execute(CommandContext& ctx) const override {
        auto* cli = ctx.cli_actor;
        auto* system = ctx.system;
        if (!cli || !system) {
            ctx.output->error("Internal error: no CLI actor or system");
            return result<void>::make();
        }

        if (ctx.args.empty()) {
            ctx.output->error("Usage: /bench start <preset>");
            ctx.output->raw("Available presets: many-actors, hot-actor, fan-in");
            return result<void>::make();
        }

        auto coord_id = find_coordinator(cli);
        if (coord_id == ActorId{0}) {
            ctx.output->error(
                "Bench coordinator not found. Is the bench_perf app running?");
            return result<void>::make();
        }

        // Check if already running by reading coordinator state
        {
            InspectStateRequest req;
            req.set_target_actor_id(coord_id.value());
            req.set_include_state(true);
            auto reply = cli->inspect(coord_id, req);
            if (reply && !reply->state_blob().empty()) {
                std::string state_str(reply->state_blob().begin(),
                                      reply->state_blob().end());
                auto kv = parse_state(state_str);
                if (kv["running"] == "yes") {
                    ctx.output->error(
                        "A benchmark is already running. Use /bench stop first.");
                    return result<void>::make();
                }
            }
        }

        // Send BenchStart message with preset name as payload
        std::string preset = ctx.args[0];
        StreamBuffer payload(
            reinterpret_cast<const uint8_t*>(preset.data()),
            reinterpret_cast<const uint8_t*>(preset.data() + preset.size()));
        system->deliver_local(
            coord_id, TypedMessage(TypeTag{0x00010102}, std::move(payload)));

        ctx.output->raw("[OK] Benchmark '" + preset + "' started.");
        return result<void>::make();
    }
};

const CommandRegistration<BenchStartCommand> kRegisterBenchStart;

// =============================================================================
// /bench stop — Stop the current run
// =============================================================================

class BenchStopCommand final : public ICommand {
  public:
    std::string_view path() const noexcept override {
        return "bench/stop";
    }
    std::string_view help_text() const noexcept override {
        return "Stop the current benchmark run";
    }
    int order() const noexcept override {
        return 501;
    }

    result<void> execute(CommandContext& ctx) const override {
        auto* cli = ctx.cli_actor;
        auto* system = ctx.system;
        if (!cli || !system) {
            ctx.output->error("Internal error");
            return result<void>::make();
        }

        auto coord_id = find_coordinator(cli);
        if (coord_id == ActorId{0}) {
            ctx.output->error("Bench coordinator not found.");
            return result<void>::make();
        }

        StreamBuffer empty;
        system->deliver_local(
            coord_id, TypedMessage(TypeTag{0x00010103}, std::move(empty)));

        ctx.output->raw("[OK] Benchmark stopped.");
        return result<void>::make();
    }
};

const CommandRegistration<BenchStopCommand> kRegisterBenchStop;

// =============================================================================
// /bench status — Show current run state
// =============================================================================

class BenchStatusCommand final : public ICommand {
  public:
    std::string_view path() const noexcept override {
        return "bench/status";
    }
    std::string_view help_text() const noexcept override {
        return "Show current benchmark run status";
    }
    int order() const noexcept override {
        return 502;
    }

    result<void> execute(CommandContext& ctx) const override {
        auto* cli = ctx.cli_actor;
        if (!cli) {
            ctx.output->error("Internal error");
            return result<void>::make();
        }

        auto coord_id = find_coordinator(cli);
        if (coord_id == ActorId{0}) {
            ctx.output->error("Bench coordinator not found.");
            return result<void>::make();
        }

        InspectStateRequest req;
        req.set_target_actor_id(coord_id.value());
        req.set_include_state(true);

        auto reply = cli->inspect(coord_id, req);
        if (!reply || reply->state_blob().empty()) {
            ctx.output->error("No response from coordinator.");
            return result<void>::make();
        }

        std::string state_str(reply->state_blob().begin(),
                              reply->state_blob().end());
        auto kv = parse_state(state_str);

        ctx.output->header("Benchmark Status");
        std::map<std::string, std::string> display;
        display["Preset"] = kv["preset"];
        display["Running"] = kv["running"];
        display["Elapsed (ms)"] = kv.count("elapsed_ms") ? kv["elapsed_ms"] : "0";
        display["Cold Workers"] = kv["cold_workers"];
        display["Hot Workers"] = kv["hot_workers"];
        if (!kv["error"].empty())
            display["Error"] = kv["error"];
        ctx.output->key_value(display);
        return result<void>::make();
    }
};

const CommandRegistration<BenchStatusCommand> kRegisterBenchStatus;

// =============================================================================
// /bench report [group] — Full latency/throughput report
// =============================================================================

class BenchReportCommand final : public ICommand {
  public:
    std::string_view path() const noexcept override {
        return "bench/report";
    }
    std::string_view help_text() const noexcept override {
        return "Show benchmark report. Usage: /bench report [hot|cold]";
    }
    int order() const noexcept override {
        return 503;
    }

    result<void> execute(CommandContext& ctx) const override {
        auto* cli = ctx.cli_actor;
        auto* system = ctx.system;
        if (!cli || !system) {
            ctx.output->error("Internal error");
            return result<void>::make();
        }

        auto coll_id = find_collector(cli);
        if (coll_id == ActorId{0}) {
            ctx.output->error("Collector not found.");
            return result<void>::make();
        }

        // Trigger stats poll via coordinator
        auto coord_id = find_coordinator(cli);
        if (coord_id != ActorId{0}) {
            StreamBuffer empty;
            system->deliver_local(
                coord_id, TypedMessage(TypeTag{0x00010104}, std::move(empty)));
        }

        // Wait a bit for collector to process
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        InspectStateRequest req;
        req.set_target_actor_id(coll_id.value());
        req.set_include_state(true);

        auto reply = cli->inspect(coll_id, req);
        if (!reply || reply->state_blob().empty()) {
            ctx.output->error("No response from collector.");
            return result<void>::make();
        }

        std::string state_str(reply->state_blob().begin(),
                              reply->state_blob().end());
        auto kv = parse_state(state_str);

        std::string filter = ctx.args.empty() ? "" : ctx.args[0];

        ctx.output->header("Benchmark Report");

        if (filter.empty() || filter == "hot") {
            ctx.output->header("Hot Actor Group");
            std::map<std::string, std::string> hot;
            hot["Samples"] = kv["hot_samples"];
            hot["P50 (us)"] = kv["hot_p50_us"];
            hot["P99 (us)"] = kv["hot_p99_us"];
            hot["P999 (us)"] = kv["hot_p999_us"];
            hot["Throughput (msg/s)"] = kv["hot_throughput_msgps"];
            ctx.output->key_value(hot);
        }

        if (filter.empty() || filter == "cold") {
            ctx.output->header("Cold Worker Group");
            std::map<std::string, std::string> cold;
            cold["Samples"] = kv["cold_samples"];
            cold["P50 (us)"] = kv["cold_p50_us"];
            cold["P99 (us)"] = kv["cold_p99_us"];
            cold["P999 (us)"] = kv["cold_p999_us"];
            cold["Throughput (msg/s)"] = kv["cold_throughput_msgps"];
            ctx.output->key_value(cold);
        }

        if (filter.empty()) {
            ctx.output->header("Totals");
            std::map<std::string, std::string> totals;
            totals["Total Throughput (msg/s)"] = kv["total_throughput_msgps"];
            totals["Elapsed (ms)"] = kv["elapsed_ms"];
            ctx.output->key_value(totals);
        }

        return result<void>::make();
    }
};

const CommandRegistration<BenchReportCommand> kRegisterBenchReport;

// =============================================================================
// /bench export — Export raw data
// =============================================================================

class BenchExportCommand final : public ICommand {
  public:
    std::string_view path() const noexcept override {
        return "bench/export";
    }
    std::string_view help_text() const noexcept override {
        return "Export benchmark data. Usage: /bench export";
    }
    int order() const noexcept override {
        return 504;
    }

    result<void> execute(CommandContext& ctx) const override {
        auto* cli = ctx.cli_actor;
        auto* system = ctx.system;
        if (!cli || !system) {
            ctx.output->error("Internal error");
            return result<void>::make();
        }

        auto coll_id = find_collector(cli);
        if (coll_id == ActorId{0}) {
            ctx.output->error("Collector not found.");
            return result<void>::make();
        }

        // Trigger stats poll
        auto coord_id = find_coordinator(cli);
        if (coord_id != ActorId{0}) {
            StreamBuffer empty;
            system->deliver_local(
                coord_id, TypedMessage(TypeTag{0x00010104}, std::move(empty)));
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        InspectStateRequest req;
        req.set_target_actor_id(coll_id.value());
        req.set_include_state(true);

        auto reply = cli->inspect(coll_id, req);
        if (!reply || reply->state_blob().empty()) {
            ctx.output->error("No data.");
            return result<void>::make();
        }

        std::string state_str(reply->state_blob().begin(),
                              reply->state_blob().end());
        auto kv = parse_state(state_str);

        // Output as simple JSON object
        std::ostringstream json;
        json << "{\n";
        bool first = true;
        for (auto& [k, v] : kv) {
            if (!first)
                json << ",\n";
            json << "  \"" << k << "\": \"" << v << "\"";
            first = false;
        }
        json << "\n}\n";
        ctx.output->raw(json.str());
        return result<void>::make();
    }
};

const CommandRegistration<BenchExportCommand> kRegisterBenchExport;

// =============================================================================
// /bench list — List available presets
// =============================================================================

class BenchListCommand final : public ICommand {
  public:
    std::string_view path() const noexcept override {
        return "bench/list";
    }
    std::string_view help_text() const noexcept override {
        return "List available benchmark presets";
    }
    int order() const noexcept override {
        return 505;
    }

    result<void> execute(CommandContext& ctx) const override {
        ctx.output->header("Available Benchmark Presets");
        ctx.output->raw(
            "  many-actors  — 5000 workers, 10us burn, 100Hz (throughput test)");
        ctx.output->raw(
            "  hot-actor    — 1 hot actor (500us, 1000Hz) + 1000 cold workers (fairness test)");
        ctx.output->raw(
            "  fan-in       — 5000 workers, 1us burn, 1000Hz → single collector (extreme fan-in stress test)");
        ctx.output->raw("");
        ctx.output->raw("Use /bench start <preset> to run.");
        return result<void>::make();
    }
};

const CommandRegistration<BenchListCommand> kRegisterBenchList;

// =============================================================================
// /bench help — Show bench commands
// =============================================================================

class BenchHelpCommand final : public ICommand {
  public:
    std::string_view path() const noexcept override {
        return "bench/help";
    }
    std::string_view help_text() const noexcept override {
        return "Show available /bench commands";
    }
    int order() const noexcept override {
        return 599;
    }

    result<void> execute(CommandContext& ctx) const override {
        ctx.output->header("Benchmark Commands");
        ctx.output->raw(
            "  /bench start <preset>  — Start (presets: many-actors, hot-actor, fan-in)");
        ctx.output->raw("  /bench stop            — Stop the current run");
        ctx.output->raw("  /bench status          — Show current run status");
        ctx.output->raw("  /bench report [group]  — Full latency/throughput report");
        ctx.output->raw("  /bench export          — Export raw data as JSON");
        ctx.output->raw("  /bench list            — List available presets");
        ctx.output->raw("  /bench help            — Show this help");
        return result<void>::make();
    }
};

const CommandRegistration<BenchHelpCommand> kRegisterBenchHelp;

} // namespace
} // namespace cli
} // namespace hpactor
