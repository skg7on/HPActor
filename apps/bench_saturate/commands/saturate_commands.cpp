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

#include <hpactor/cli/cli_local_actor.hpp>
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
    auto actors = cli->enumerate("SaturateCoordinatorActor");
    if (actors.empty())
        return ActorId{0};
    return ActorId{actors[0].actor_id};
}

// =============================================================================
// Helper: find collector actor by type name
// =============================================================================

static ActorId find_collector(CliActor* cli) {
    auto actors = cli->enumerate("SaturateCollectorActor");
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
// /saturate start <preset>
// =============================================================================

class SaturateStartCommand final : public ICommand {
  public:
    std::string_view path() const noexcept override {
        return "saturate/start";
    }
    std::string_view help_text() const noexcept override {
        return "Start a saturation run. Usage: /saturate start <preset>";
    }
    int order() const noexcept override {
        return 600;
    }

    result<void> execute(CommandContext& ctx) const override {
        auto* cli = ctx.cli_actor;
        auto* system = ctx.system;
        if (!cli || !system) {
            ctx.output->error("Internal error: no CLI actor or system");
            return result<void>::make();
        }

        if (ctx.args.empty()) {
            ctx.output->error("Usage: /saturate start <preset>");
            ctx.output->raw("Available presets: quick-saturate, deep-saturate, "
                            "alloc-stress, mixed-load, fan-in-extreme, "
                            "fan-out-burst");
            return result<void>::make();
        }

        auto coord_id = find_coordinator(cli);
        if (coord_id == ActorId{0}) {
            ctx.output->error("Saturate coordinator not found. Is the "
                              "bench_saturate app running?");
            return result<void>::make();
        }

        // Check if already running
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
                    ctx.output->error("A run is already in progress. Use "
                                      "/saturate stop first.");
                    return result<void>::make();
                }
            }
        }

        std::string preset = ctx.args[0];
        StreamBuffer payload(
            reinterpret_cast<const uint8_t*>(preset.data()),
            reinterpret_cast<const uint8_t*>(preset.data() + preset.size()));
        system->deliver_local(
            coord_id, TypedMessage(TypeTag{0x00010200}, std::move(payload)));

        ctx.output->raw("[OK] Saturation run '" + preset + "' started.");
        return result<void>::make();
    }
};

const CommandRegistration<SaturateStartCommand> kRegisterSaturateStart;

// =============================================================================
// /saturate stop
// =============================================================================

class SaturateStopCommand final : public ICommand {
  public:
    std::string_view path() const noexcept override {
        return "saturate/stop";
    }
    std::string_view help_text() const noexcept override {
        return "Stop the current saturation run";
    }
    int order() const noexcept override {
        return 601;
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
            ctx.output->error("Saturate coordinator not found.");
            return result<void>::make();
        }

        StreamBuffer empty;
        system->deliver_local(
            coord_id, TypedMessage(TypeTag{0x00010201}, std::move(empty)));

        ctx.output->raw("[OK] Saturation run stopped.");
        return result<void>::make();
    }
};

const CommandRegistration<SaturateStopCommand> kRegisterSaturateStop;

// =============================================================================
// /saturate status
// =============================================================================

class SaturateStatusCommand final : public ICommand {
  public:
    std::string_view path() const noexcept override {
        return "saturate/status";
    }
    std::string_view help_text() const noexcept override {
        return "Show current saturation run status";
    }
    int order() const noexcept override {
        return 602;
    }

    result<void> execute(CommandContext& ctx) const override {
        auto* cli = ctx.cli_actor;
        if (!cli) {
            ctx.output->error("Internal error");
            return result<void>::make();
        }

        auto coord_id = find_coordinator(cli);
        if (coord_id == ActorId{0}) {
            ctx.output->error("Saturate coordinator not found.");
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

        ctx.output->header("Saturation Benchmark Status");
        std::map<std::string, std::string> display;
        display["Preset"] = kv["preset"];
        display["Phase"] = kv["phase"];
        display["Running"] = kv["running"];
        display["Current Rate (msg/s)"] = kv["current_rate_msgps"];
        display["Drop Rate (%)"] = kv["drop_rate_pct"];
        display["Saturation Ceiling"] = kv.count("saturation_ceiling")
                                            ? kv["saturation_ceiling"]
                                            : "not yet discovered";
        display["Elapsed (ms)"] = kv.count("elapsed_ms") ? kv["elapsed_ms"] : "0";
        display["Senders"] = kv["senders"];
        display["Receivers"] = kv["receivers"];
        if (!kv["error"].empty())
            display["Error"] = kv["error"];
        ctx.output->key_value(display);
        return result<void>::make();
    }
};

const CommandRegistration<SaturateStatusCommand> kRegisterSaturateStatus;

// =============================================================================
// /saturate report [detail]
// =============================================================================

class SaturateReportCommand final : public ICommand {
  public:
    std::string_view path() const noexcept override {
        return "saturate/report";
    }
    std::string_view help_text() const noexcept override {
        return "Show saturation report. Usage: /saturate report "
               "[detail]";
    }
    int order() const noexcept override {
        return 603;
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
                coord_id, TypedMessage(TypeTag{0x00010203}, std::move(empty)));
        }

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

        ctx.output->header("Saturation Benchmark Report");

        ctx.output->header("Throughput & Drops");
        std::map<std::string, std::string> overview;
        overview["Total Sent"] = kv["total_sent"];
        overview["Total Received"] = kv["total_received"];
        overview["Total Dropped"] = kv["total_dropped"];
        overview["Drop Rate (%)"] = kv["drop_rate_pct"];
        overview["Throughput (msg/s)"] = kv["throughput_msgps"];
        overview["Elapsed (ms)"] = kv["elapsed_ms"];
        ctx.output->key_value(overview);

        ctx.output->header("Latency");
        std::map<std::string, std::string> lat;
        lat["P50 (us)"] = kv["p50_us"];
        lat["P99 (us)"] = kv["p99_us"];
        lat["P999 (us)"] = kv["p999_us"];
        ctx.output->key_value(lat);

        bool detail = !ctx.args.empty() && ctx.args[0] == "detail";
        if (detail) {
            ctx.output->header("Drop Curve");
            ctx.output->raw("  curve_points=" + kv["curve_points"] +
                            " (use /saturate export for full "
                            "data)");
        }

        return result<void>::make();
    }
};

const CommandRegistration<SaturateReportCommand> kRegisterSaturateReport;

// =============================================================================
// /saturate export [json|csv]
// =============================================================================

class SaturateExportCommand final : public ICommand {
  public:
    std::string_view path() const noexcept override {
        return "saturate/export";
    }
    std::string_view help_text() const noexcept override {
        return "Export benchmark data. Usage: /saturate export "
               "[json|csv]";
    }
    int order() const noexcept override {
        return 604;
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

        auto coord_id = find_coordinator(cli);
        if (coord_id != ActorId{0}) {
            StreamBuffer empty;
            system->deliver_local(
                coord_id, TypedMessage(TypeTag{0x00010203}, std::move(empty)));
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

        std::string fmt = ctx.args.empty() ? "json" : ctx.args[0];
        if (fmt == "csv") {
            std::ostringstream csv;
            csv << "total_sent,total_received,total_dropped,"
                << "drop_rate_pct,"
                << "p50_us,p99_us,p999_us,throughput_msgps,"
                << "elapsed_ms\n";
            csv << kv["total_sent"] << "," << kv["total_received"] << ","
                << kv["total_dropped"] << "," << kv["drop_rate_pct"] << ","
                << kv["p50_us"] << "," << kv["p99_us"] << "," << kv["p999_us"]
                << "," << kv["throughput_msgps"] << "," << kv["elapsed_ms"]
                << "\n";
            ctx.output->raw(csv.str());
        } else {
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
        }
        return result<void>::make();
    }
};

const CommandRegistration<SaturateExportCommand> kRegisterSaturateExport;

// =============================================================================
// /saturate list
// =============================================================================

class SaturateListCommand final : public ICommand {
  public:
    std::string_view path() const noexcept override {
        return "saturate/list";
    }
    std::string_view help_text() const noexcept override {
        return "List available saturation presets";
    }
    int order() const noexcept override {
        return 605;
    }

    result<void> execute(CommandContext& ctx) const override {
        ctx.output->header("Available Saturation Presets");
        ctx.output->raw("  quick-saturate  — 100→10, 16B small, fast "
                        "ceiling find (~30s)");
        ctx.output->raw("  deep-saturate   — 1000→100, 16B small, thorough "
                        "curve (~60s)");
        ctx.output->raw("  alloc-stress    — 500→50, 1KB-64KB junk, "
                        "allocator pressure");
        ctx.output->raw("  mixed-load      — 500→50, 80/20 mixed, realistic "
                        "workload");
        ctx.output->raw("  fan-in-extreme  — 5000→1, 16B small, extreme "
                        "contention");
        ctx.output->raw("  fan-out-burst   — 10→1000, 1KB-16KB junk, broad "
                        "fan-out");
        ctx.output->raw("");
        ctx.output->raw("Use /saturate start <preset> to run.");
        return result<void>::make();
    }
};

const CommandRegistration<SaturateListCommand> kRegisterSaturateList;

// =============================================================================
// /saturate probe
// =============================================================================

class SaturateProbeCommand final : public ICommand {
  public:
    std::string_view path() const noexcept override {
        return "saturate/probe";
    }
    std::string_view help_text() const noexcept override {
        return "Show system hardware probe results";
    }
    int order() const noexcept override {
        return 606;
    }

    result<void> execute(CommandContext& ctx) const override {
        auto* cli = ctx.cli_actor;
        auto* system = ctx.system;
        if (!cli || !system) {
            ctx.output->error("Internal error");
            return result<void>::make();
        }

        ctx.output->header("System Probe");
        std::map<std::string, std::string> probe;

        unsigned int cores = std::thread::hardware_concurrency();
        probe["Logical Cores"] = std::to_string(cores);
        probe["Scheduler Threads"] =
            std::to_string(system->config().scheduler_threads);
        probe["Wake Strategy"] = "Polling";

        ctx.output->key_value(probe);
        ctx.output->raw("Note: Full system probe available at app startup.");
        return result<void>::make();
    }
};

const CommandRegistration<SaturateProbeCommand> kRegisterSaturateProbe;

// =============================================================================
// /saturate help
// =============================================================================

class SaturateHelpCommand final : public ICommand {
  public:
    std::string_view path() const noexcept override {
        return "saturate/help";
    }
    std::string_view help_text() const noexcept override {
        return "Show available /saturate commands";
    }
    int order() const noexcept override {
        return 699;
    }

    result<void> execute(CommandContext& ctx) const override {
        ctx.output->header("Saturate Commands");
        ctx.output->raw("  /saturate start <preset>  — Start a saturation "
                        "run");
        ctx.output->raw("  /saturate stop            — Stop the current run");
        ctx.output->raw("  /saturate status          — Show current run "
                        "status");
        ctx.output->raw("  /saturate report [detail] — Full saturation "
                        "report");
        ctx.output->raw("  /saturate export [json|csv] — Export results");
        ctx.output->raw("  /saturate list            — List available presets");
        ctx.output->raw("  /saturate probe           — Show system probe");
        ctx.output->raw("  /saturate help            — Show this help");
        return result<void>::make();
    }
};

const CommandRegistration<SaturateHelpCommand> kRegisterSaturateHelp;

} // namespace
} // namespace cli
} // namespace hpactor
