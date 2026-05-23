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

namespace hpactor::config {
namespace {

class QuarantineConfigParser final : public ITomlSystemConfigParser {
  public:
    static constexpr std::string_view kName = "system.quarantine";
    static constexpr int kOrder = 70;

    std::string_view name() const noexcept override {
        return kName;
    }
    int order() const noexcept override {
        return kOrder;
    }

    result<void> parse(const TomlTableView& system, SystemDef& out,
                       TomlParseContext& /*ctx*/) const override {
        auto qt = system.table("quarantine");
        if (!qt.valid())
            return result<void>::make();

        auto& defaults = out.quarantine_defaults;
        defaults.enabled = qt.read_bool("default_enabled", false);
        defaults.escalate_on_max_restarts =
            qt.read_bool("default_escalate_on_max_restarts", true);
        defaults.failure_rate_threshold =
            qt.read_uint32("default_failure_rate_threshold", 0);
        defaults.timeout_rate_threshold =
            qt.read_uint32("default_timeout_rate_threshold", 0);
        defaults.mailbox_pressure_threshold = static_cast<float>(
            qt.read_double("default_mailbox_pressure_threshold", 0.0));
        defaults.cooldown_period = std::chrono::milliseconds(static_cast<int64_t>(
            qt.read_uint32("default_cooldown_period_ms", 30000)));
        defaults.observation_window =
            std::chrono::milliseconds(static_cast<int64_t>(
                qt.read_uint32("default_observation_window_ms", 10000)));
        defaults.max_circuit_trips = qt.read_uint32("default_max_circuit_trips", 3);

        return result<void>::make();
    }
};

const TomlSystemParserRegistration<QuarantineConfigParser> kRegisterQuarantineConfigParser;

} // namespace
} // namespace hpactor::config
