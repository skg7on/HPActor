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

/// \brief Parse [system.passivation] TOML section.
///
/// Reads system-level passivation defaults. The sub-table is optional;
/// when absent all defaults are used. Per-actor overrides in [[actor]]
/// blocks are handled separately.
class PassivationConfigParser final : public ITomlSystemConfigParser {
  public:
    static constexpr std::string_view kName = "system.passivation";
    static constexpr int kOrder = 10;

    std::string_view name() const noexcept override {
        return kName;
    }
    int order() const noexcept override {
        return kOrder;
    }

    result<void> parse(const TomlTableView& st, SystemDef& /*out*/,
                       TomlParseContext& /*ctx*/) const override {
        bool found = false;
        st.for_each_subtable("passivation", [&](std::string_view /*name*/,
                                                const TomlTableView& pv) {
            found = true;

            pv.read_bool("enabled");
            pv.read_uint32("default_idle_timeout_ms");
            pv.read_bool("memory_pressure_enabled");
            pv.read_uint32("memory_pressure_high_threshold_pct");
            pv.read_uint32("memory_pressure_poll_interval_ms");
            pv.read_uint32("max_reactivation_queue_depth");
            pv.read_uint32("drain_timeout_ms");

            // Parse [system.passivation.store] sub-table
            pv.for_each_subtable("store", [&](std::string_view /*name*/,
                                              const TomlTableView& store) {
                store.read_string("type");
                store.read_string("directory");
            });
        });

        (void)found; // passivation section is optional
        return result<void>::make();
    }
};

const TomlSystemParserRegistration<PassivationConfigParser> kRegisterPassivationConfigParser;

} // anonymous namespace
} // namespace hpactor::config
