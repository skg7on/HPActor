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

/// \brief Parse [system.cluster.leadership] TOML section.
///
/// Reads cluster leadership election configuration from the system TOML table.
/// The sub-table is optional; when absent all defaults are used.
/// ClusterRuntimeImpl reads these values at runtime via TOML access.
class ClusterLeadershipConfigParser final : public ITomlSystemConfigParser {
  public:
    static constexpr std::string_view kName = "system.cluster.leadership";
    static constexpr int kOrder = 90;

    std::string_view name() const noexcept override {
        return kName;
    }
    int order() const noexcept override {
        return kOrder;
    }

    result<void> parse(const TomlTableView& st, SystemDef& /*out*/,
                       TomlParseContext& /*ctx*/) const override {
        auto cluster = st.table("cluster");
        if (!cluster.valid())
            return result<void>::make();
        auto leadership = cluster.table("leadership");
        if (!leadership.valid())
            return result<void>::make();

        // Election mode: "local" | "external" | "disabled"
        leadership.read_string("mode", "local");
        // External backend: "etcd" | "consul" | "raft"
        leadership.read_string("backend", "etcd");
        // Lease TTL in milliseconds
        leadership.read_uint32("lease_ttl_ms", 10000);
        // Lease renew interval in milliseconds
        leadership.read_uint32("renew_interval_ms", 3000);
        // Renew deadline before lease expiry in milliseconds
        leadership.read_uint32("renew_deadline_ms", 7000);
        // Grace period after stepping down in milliseconds
        leadership.read_uint32("step_down_grace_ms", 1000);
        // Whether to fail closed when backend is unavailable
        leadership.read_bool("fail_closed_on_backend_unavailable", true);

        return result<void>::make();
    }
};

const TomlSystemParserRegistration<ClusterLeadershipConfigParser> kRegisterClusterLeadershipConfigParser;

} // anonymous namespace
} // namespace hpactor::config
