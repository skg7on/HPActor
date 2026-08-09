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

class CliConfigParser final : public ITomlSystemConfigParser {
  public:
    static constexpr std::string_view kName = "system.cli";
    static constexpr int kOrder = 65;

    std::string_view name() const noexcept override {
        return kName;
    }
    int order() const noexcept override {
        return kOrder;
    }

    result<void> parse(const TomlTableView& system, SystemDef& out,
                       TomlParseContext& /*ctx*/) const override {
        auto ct = system.table("cli");
        if (!ct.valid())
            return result<void>::make();

        out.cli.enabled = ct.read_bool("enabled", true);
        out.cli.listen_path = ct.read_string("listen_path", "");
        out.cli.tcp_port = static_cast<uint16_t>(ct.read_uint32("tcp_port", 0));
        out.cli.default_format = ct.read_string("default_format", "pretty");
        out.cli.page_size = ct.read_uint32("page_size", 50);

        return result<void>::make();
    }
};

const TomlSystemParserRegistration<CliConfigParser> kRegisterCliConfigParser;

} // anonymous namespace
} // namespace hpactor::config
