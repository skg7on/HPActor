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

#include <hpactor/config/toml_config_parser.hpp>
#include <hpactor/config/toml_parser_registry.hpp>

#include <chrono>

namespace hpactor::config {
namespace {

class ProcessConfigParser final : public ITomlSystemConfigParser {
  public:
    static constexpr std::string_view kName = "system.process";
    static constexpr int kOrder = 10;

    std::string_view name() const noexcept override {
        return kName;
    }
    int order() const noexcept override {
        return kOrder;
    }

    result<void> parse(const TomlTableView& system, SystemDef& out,
                       TomlParseContext& /*ctx*/) const override {
        auto pt = system.table("process");
        if (!pt.valid())
            return result<void>::make();

        auto mode_str = pt.read_string("mode", "foreground");
        out.process.mode = process::ProcessConfig::parse_mode(mode_str);
        out.process.pidfile_path = pt.read_string("pidfile", "");
        out.process.redirect_stdio = pt.read_bool("redirect_stdio");
        out.process.log_file = pt.read_string("log_file", "");
        out.process.working_directory = pt.read_string("working_directory", "/");
        out.process.watchdog_interval = std::chrono::milliseconds(
            static_cast<int64_t>(pt.read_uint32("watchdog_interval_ms", 0)));

        // [system.process.health] subsection
        auto hc = pt.table("health");
        if (hc.valid()) {
            auto& h = out.process.health_check;
            h.enabled = hc.read_bool("enabled", true);
            h.scheduler_liveness_enabled =
                hc.read_bool("scheduler_liveness_enabled", true);
            h.system_actor_health_enabled =
                hc.read_bool("system_actor_health_enabled", true);
            h.dlq_growth_enabled = hc.read_bool("dlq_growth_enabled", true);
            h.memory_pressure_enabled =
                hc.read_bool("memory_pressure_enabled", true);
            h.scheduler_progress_deadline_sec =
                hc.read_uint32("scheduler_progress_deadline_sec", 30);
            h.dlq_depth_warning_pct = hc.read_uint32("dlq_depth_warning_pct", 80);
            h.dlq_depth_critical_pct = hc.read_uint32("dlq_depth_critical_pct", 95);
            h.dlq_lost_rate_per_minute =
                hc.read_uint32("dlq_lost_rate_per_minute", 10);
            h.memory_warning_pct =
                static_cast<uint8_t>(hc.read_uint32("memory_warning_pct", 85));
            h.memory_critical_pct =
                static_cast<uint8_t>(hc.read_uint32("memory_critical_pct", 95));
        }

        return result<void>::make();
    }
};

const TomlSystemParserRegistration<ProcessConfigParser> kRegisterProcessConfigParser;

} // anonymous namespace
} // namespace hpactor::config
