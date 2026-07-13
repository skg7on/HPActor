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

#include <hpactor/config/name_resolution_config.hpp>
#include <hpactor/config/toml_config_parser.hpp>
#include <hpactor/config/toml_parser_registry.hpp>
#include <hpactor/config/toml_table_view.hpp>

namespace hpactor::config {
namespace {

/// \brief Parse [system.name_resolution] TOML section.
///
/// Reads distributed name resolution configuration. The sub-table is
/// optional; when absent all defaults are used.
class NameResolutionConfigParser final : public ITomlSystemConfigParser {
  public:
    static constexpr std::string_view kName = "system.name_resolution";
    static constexpr int kOrder = 110;

    std::string_view name() const noexcept override {
        return kName;
    }
    int order() const noexcept override {
        return kOrder;
    }

    result<void> parse(const TomlTableView& system, SystemDef& out,
                       TomlParseContext& /*ctx*/) const override {
        auto nr = system.table("name_resolution");
        if (!nr.valid())
            return result<void>::make();

        out.name_resolution.enabled =
            nr.read_bool("enabled", false);
        out.name_resolution.resolve_timeout_ms =
            nr.read_uint32("resolve_timeout_ms", 2000);
        out.name_resolution.register_timeout_ms =
            nr.read_uint32("register_timeout_ms", 5000);
        out.name_resolution.cache_ttl_seconds =
            nr.read_uint32("cache_ttl_seconds", 30);
        out.name_resolution.virtual_nodes =
            nr.read_uint32("virtual_nodes", 100);

        if (!out.name_resolution.valid())
            return result<void>::make(
                error(errors::invalid_argument,
                      "name_resolution config validation failed"));

        return result<void>::make();
    }
};

const TomlSystemParserRegistration<NameResolutionConfigParser>
    kRegisterNameResolutionConfigParser;

} // anonymous namespace
} // namespace hpactor::config
