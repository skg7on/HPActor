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

class PythonBindingConfigParser final : public ITomlSystemConfigParser {
  public:
    static constexpr std::string_view kName = "system.python";
    static constexpr int kOrder = 45;

    std::string_view name() const noexcept override {
        return kName;
    }
    int order() const noexcept override {
        return kOrder;
    }

    result<void> parse(const TomlTableView& system, SystemDef& out,
                       TomlParseContext& /*ctx*/) const override {
        auto pt = system.table("python");
        if (!pt.valid())
            return result<void>::make();

        out.python.enabled = pt.read_bool("enabled", false);
        out.python.dispatch_queue_capacity =
            pt.read_uint32("dispatch_queue_capacity", 65536);
        out.python.command_queue_capacity =
            pt.read_uint32("command_queue_capacity", 16384);
        out.python.completion_queue_capacity =
            pt.read_uint32("completion_queue_capacity", 16384);
        out.python.max_actor_bindings = pt.read_uint32("max_actor_bindings", 65536);
        out.python.max_dispatch_per_tick =
            pt.read_uint32("max_dispatch_per_tick", 256);
        out.python.max_commands_per_turn =
            pt.read_uint32("max_commands_per_turn", 256);
        out.python.loop_lag_unready_ms =
            pt.read_uint32("loop_lag_unready_ms", 5000);
        out.python.handler_shutdown_timeout_ms =
            pt.read_uint32("handler_shutdown_timeout_ms", 10000);
        out.python.trace_handler_spans = pt.read_bool("trace_handler_spans", true);

        return out.python.validate();
    }
};

const TomlSystemParserRegistration<PythonBindingConfigParser> kRegisterPythonBindingConfigParser;

} // anonymous namespace
} // namespace hpactor::config
