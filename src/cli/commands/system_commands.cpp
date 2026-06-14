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
#include <hpactor/cli/cli_server_actor.hpp>
#include <hpactor/cli/command_registry.hpp>
#include <hpactor/cli/output_formatter.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/mem/memory_config.hpp>
#include <hpactor/mem/memory_region.hpp>
#include <hpactor/mem/memory_tracker.hpp>
#include <hpactor/types/types.hpp>

#include "command_utils.hpp"

#include <cstdio>
#include <map>
#include <string>

namespace hpactor {
namespace cli {
namespace {

class SystemStatsCommand final : public ICommand {
  public:
    std::string_view path() const noexcept override {
        return "system/stats";
    }
    std::string_view help_text() const noexcept override {
        return "System-wide statistics";
    }
    int order() const noexcept override {
        return 100;
    }

    result<void> execute(CommandContext& ctx) const override {
        ctx.output->header("System Statistics");
        auto* sys = ctx.system;
        auto* cli = ctx.cli_actor;
        if (!sys) {
            ctx.output->error("Internal error: no actor system");
            return result<void>::make();
        }
        std::map<std::string, std::string> kv;
        kv["Total actors"] = std::to_string(sys->actor_count());
        if (auto* sched = sys->scheduler()) {
            kv["Scheduler threads"] = std::to_string(sched->worker_count());
        }
        if (cli) {
            kv["CLI enabled"] = cli->config().enabled ? "yes" : "no";
            kv["CLI format"] = cli->config().default_format;
        }
        ctx.output->key_value(kv);
        return result<void>::make();
    }
};

class SystemMemoryCommand final : public ICommand {
  public:
    std::string_view path() const noexcept override {
        return "system/memory";
    }
    std::string_view help_text() const noexcept override {
        return "Memory subsystem stats";
    }
    int order() const noexcept override {
        return 200;
    }

    result<void> execute(CommandContext& ctx) const override {
        ctx.output->header("Memory Regions");

        auto& reg = mem::MemoryRegionRegistry::instance();

        std::vector<std::string> cols = {"Region",     "Active", "Limit",
                                         "Pressure",   "Allocs", "Frees",
                                         "Corruptions"};
        std::vector<std::vector<std::string>> rows;

        // Iterate over all 6 region types
        static constexpr mem::RegionType kRegions[] = {
            mem::RegionType::kActor,     mem::RegionType::kMessage,
            mem::RegionType::kCoroutine, mem::RegionType::kNetwork,
            mem::RegionType::kInternal,  mem::RegionType::kHibernate};

        for (auto region : kRegions) {
            auto snap = reg.snapshot(region);
            rows.push_back({
                mem::to_string(region),
                format_bytes(snap.active_bytes),
                snap.limit.hard_limit_bytes > 0
                    ? format_bytes(snap.limit.hard_limit_bytes)
                    : "unlimited",
                mem::to_string(snap.pressure),
                std::to_string(snap.alloc_count),
                std::to_string(snap.free_count),
                std::to_string(snap.corruption_events),
            });
        }

        ctx.output->table(cols, rows);

        // ── Per-actor memory table ──────────────────────────────────────
        if constexpr (mem::kMemoryTrackingEnabled) {
            ctx.output->header("Per-Actor Memory");

            std::vector<std::string> mem_cols = {"ID",   "Type",   "Current",
                                                 "Peak", "Allocs", "Frees"};
            std::vector<std::vector<std::string>> mem_rows;

            auto* sys = ctx.system;
            if (sys) {
                auto& tracker = mem::MemoryTracker::instance();
                sys->for_each_actor([&](ActorId actor_id, AbstractActor& actor) {
                    mem::ActorMemoryStats stats;
                    tracker.snapshot(actor_id, stats);

                    char id_buf[32];
                    snprintf(id_buf, sizeof(id_buf), "0x%04llX",
                             static_cast<unsigned long long>(actor_id.value()));

                    auto meta = actor.to_metadata();
                    mem_rows.push_back({
                        id_buf,
                        meta.actor_type,
                        format_bytes(stats.current_bytes),
                        format_bytes(stats.peak_bytes),
                        std::to_string(stats.alloc_count),
                        std::to_string(stats.free_count),
                    });
                });
            }
            ctx.output->table(mem_cols, mem_rows);
        } else {
            ctx.output->raw("Per-actor memory tracking is disabled at compile time.");
        }

        return result<void>::make();
    }
};

class SystemListCommand final : public ICommand {
  public:
    std::string_view path() const noexcept override {
        return "system/list";
    }
    std::string_view help_text() const noexcept override {
        return "List system actors";
    }
    int order() const noexcept override {
        return 300;
    }

    result<void> execute(CommandContext& ctx) const override {
        ctx.output->header("System Actors");
        auto* sys = ctx.system;
        if (!sys) {
            ctx.output->error("Internal error: no actor system");
            return result<void>::make();
        }

        std::vector<std::string> cols = {"ID", "Type", "State"};
        std::vector<std::vector<std::string>> rows;

        sys->for_each_actor([&](ActorId actor_id, AbstractActor& actor) {
            char id_buf[32];
            snprintf(id_buf, sizeof(id_buf), "0x%04llX",
                     static_cast<unsigned long long>(actor_id.value()));
            auto meta = actor.to_metadata();
            rows.push_back({id_buf, meta.actor_type, meta.state});
        });

        ctx.output->table(cols, rows);
        return result<void>::make();
    }
};

class SystemUptimeCommand final : public ICommand {
  public:
    std::string_view path() const noexcept override {
        return "system/uptime";
    }
    std::string_view help_text() const noexcept override {
        return "Show actor system uptime";
    }
    int order() const noexcept override {
        return 80;
    }

    result<void> execute(CommandContext& ctx) const override {
        auto* sys = ctx.system;
        if (!sys) {
            ctx.output->error("Internal error: no actor system");
            return result<void>::make();
        }

        auto uptime_ms = sys->uptime();
        auto total_seconds =
            std::chrono::duration_cast<std::chrono::seconds>(uptime_ms).count();

        auto days = total_seconds / 86400;
        auto hours = (total_seconds % 86400) / 3600;
        auto minutes = (total_seconds % 3600) / 60;
        auto seconds = total_seconds % 60;

        char buf[128];
        if (days > 0) {
            snprintf(buf, sizeof(buf), "%lldd %lldh %lldm %llds",
                     static_cast<long long>(days), static_cast<long long>(hours),
                     static_cast<long long>(minutes),
                     static_cast<long long>(seconds));
        } else if (hours > 0) {
            snprintf(buf, sizeof(buf), "%lldh %lldm %llds",
                     static_cast<long long>(hours), static_cast<long long>(minutes),
                     static_cast<long long>(seconds));
        } else if (minutes > 0) {
            snprintf(buf, sizeof(buf), "%lldm %llds",
                     static_cast<long long>(minutes),
                     static_cast<long long>(seconds));
        } else {
            snprintf(buf, sizeof(buf), "%llds", static_cast<long long>(seconds));
        }

        ctx.output->header("System Uptime");
        std::map<std::string, std::string> kv;
        kv["Uptime"] = buf;
        kv["Uptime (ms)"] = std::to_string(uptime_ms.count());
        kv["Uptime (seconds)"] = std::to_string(total_seconds);
        ctx.output->key_value(kv);
        return result<void>::make();
    }
};

class SystemDrainCommand final : public ICommand {
  public:
    std::string_view path() const noexcept override {
        return "system/drain";
    }
    std::string_view help_text() const noexcept override {
        return "Graceful node shutdown";
    }
    int order() const noexcept override {
        return 400;
    }

    result<void> execute(CommandContext& ctx) const override {
        auto* sys = ctx.system;
        if (!sys) {
            ctx.output->error("Internal error: no actor system");
            return result<void>::make();
        }
        auto shutdown_result = sys->shutdown();
        if (shutdown_result.has_value()) {
            ctx.output->raw("Shutdown complete");
        } else {
            ctx.output->error("Shutdown failed");
        }
        return result<void>::make();
    }
};

class SystemDrainStatusCommand final : public ICommand {
  public:
    std::string_view path() const noexcept override {
        return "system/drain/status";
    }
    std::string_view help_text() const noexcept override {
        return "Show shutdown progress";
    }
    int order() const noexcept override {
        return 410;
    }

    result<void> execute(CommandContext& ctx) const override {
        auto* sys = ctx.system;
        if (!sys) {
            ctx.output->error("Internal error: no actor system");
            return result<void>::make();
        }
        ctx.output->raw("Shutdown phase: " +
                        std::to_string(static_cast<int>(sys->shutdown_phase())));
        ctx.output->raw("Actors live: " + std::to_string(sys->actor_count()));
        return result<void>::make();
    }
};

class SystemStopCommand final : public ICommand {
  public:
    std::string_view path() const noexcept override {
        return "system/stop/<actor_id>";
    }
    std::string_view help_text() const noexcept override {
        return "Graceful stop of an actor [--force]";
    }
    int order() const noexcept override {
        return 500;
    }

    result<void> execute(CommandContext& ctx) const override {
        auto id_str = ctx.get_param("<actor_id>");
        if (!id_str) {
            ctx.output->error("Missing actor ID (usage: /system stop "
                              "<actor_id>)");
            return result<void>::make();
        }
        ActorId target_id = parse_actor_id(*id_str);
        if (target_id == ActorId{0}) {
            ctx.output->error("Invalid actor ID: " + *id_str);
            return result<void>::make();
        }

        auto* sys = ctx.system;
        if (!sys || (!ctx.cli_actor && !ctx.cli_server_actor)) {
            ctx.output->error("Internal error");
            return result<void>::make();
        }

        if (ctx.has_flag("force")) {
            sys->set_drain_config(target_id,
                                  DrainConfig{DrainPolicy::ImmediateStop});
        }

        auto actor = sys->get_actor(target_id);
        if (!actor) {
            ctx.output->error("Actor not found: " + *id_str);
            return result<void>::make();
        }
        if (ctx.cli_actor)
            ctx.cli_actor->context()->stop(target_id);
        else
            ctx.cli_server_actor->context()->stop(target_id);
        ctx.output->raw("Drain initiated for actor " + *id_str);
        return result<void>::make();
    }
};

const CommandRegistration<SystemUptimeCommand> kRegisterSystemUptime;
const CommandRegistration<SystemStatsCommand> kRegisterSystemStats;
const CommandRegistration<SystemMemoryCommand> kRegisterSystemMemory;
const CommandRegistration<SystemListCommand> kRegisterSystemList;
const CommandRegistration<SystemDrainCommand> kRegisterSystemDrain;
const CommandRegistration<SystemDrainStatusCommand> kRegisterSystemDrainStatus;
const CommandRegistration<SystemStopCommand> kRegisterSystemStop;

} // anonymous namespace
} // namespace cli
} // namespace hpactor