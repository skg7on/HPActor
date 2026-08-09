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

class MetricsConfigParser final : public ITomlSystemConfigParser {
  public:
    static constexpr std::string_view kName = "system.metrics";
    static constexpr int kOrder = 40;

    std::string_view name() const noexcept override {
        return kName;
    }
    int order() const noexcept override {
        return kOrder;
    }

    result<void> parse(const TomlTableView& system, SystemDef& out,
                       TomlParseContext& /*ctx*/) const override {
        auto mt = system.table("metrics");
        if (!mt.valid())
            return result<void>::make();

        out.metrics_enabled = mt.read_bool("enabled", true);
        out.metrics_ring_buffer_capacity =
            mt.read_uint32("ring_buffer_capacity", 65536);
        out.metrics_path = mt.read_string("metrics_path", "/metrics");

        return result<void>::make();
    }
};

const TomlSystemParserRegistration<MetricsConfigParser> kRegisterMetricsConfigParser;

} // anonymous namespace
} // namespace hpactor::config
