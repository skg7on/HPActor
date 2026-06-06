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

#include <hpactor/config/rate_limiting_config.hpp>
#include <hpactor/config/toml_parser_registry.hpp>
#include <hpactor/config/toml_table_view.hpp>

namespace hpactor::config {
namespace {

struct RateLimitingSystemParser {
    static constexpr std::string_view section_name = "rate_limiting";

    void parse(const TomlTableView& table, TopologyModel& model) const {
        (void)table;
        (void)model;
        // Full parsing implementation deferred to Phase 8.
        // The section structure is:
        // [system.rate_limiting]
        // enabled = false
        // default_rate = 100.0
        // default_burst = 10
        //
        // [system.admission]
        // enabled = false
        // ...
        //
        // Per-actor override is read during bootstrap from the actor def.
    }
};

const TomlSystemParserRegistration<RateLimitingSystemParser> kRegister{};

} // anonymous namespace
} // namespace hpactor::config
