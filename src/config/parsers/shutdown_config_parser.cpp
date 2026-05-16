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

class ShutdownConfigParser final : public ITomlSystemConfigParser {
  public:
    static constexpr std::string_view kName = "system.shutdown";
    static constexpr int kOrder = 110;

    std::string_view name() const noexcept override {
        return kName;
    }
    int order() const noexcept override {
        return kOrder;
    }

    result<void> parse(const TomlTableView& system, SystemDef& out,
                       TomlParseContext& /*ctx*/) const override {
        auto st = system.table("shutdown");
        if (!st.valid())
            return result<void>::make();

        out.default_drain_policy = st.read_string("drain_policy", "Drain");
        out.default_drain_timeout_ms = st.read_uint32("drain_timeout_ms", 30000);
        out.shutdown_ingress_timeout_ms =
            st.read_uint32("ingress_timeout_ms", 5000);
        out.shutdown_cluster_leave_timeout_ms =
            st.read_uint32("cluster_leave_timeout_ms", 10000);
        out.shutdown_force_after_timeout =
            st.read_bool("force_after_timeout", true);
        return result<void>::make();
    }
};

const TomlSystemParserRegistration<ShutdownConfigParser> kRegisterShutdownConfigParser;

} // anonymous namespace
} // namespace hpactor::config
