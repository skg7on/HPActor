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

#include <hpactor/actor/actor_system.hpp>
#include <hpactor/cli/command_registry.hpp>
#include <hpactor/cli/output_formatter.hpp>
#include <hpactor/fault/fault_controller.hpp>
#include <hpactor/fault/fault_point.hpp>

#include <map>
#include <string>

namespace hpactor {
namespace cli {
namespace {

class FaultStatusCommand final : public ICommand {
  public:
    std::string_view path() const noexcept override {
        return "fault/status";
    }
    std::string_view help_text() const noexcept override {
        return "Show fault injection status";
    }
    int order() const noexcept override {
        return 90;
    }

    result<void> execute(CommandContext& ctx) const override {
        if (ctx.system_host &&
            ctx.system_host->execute_path("fault/status", {}, {}, *ctx.output))
            return result<void>::make();
        // FALLBACK: existing inline logic (for tests without a host)
        auto* system = ctx.system;
        if (!system) {
            ctx.output->error("No actor system available");
            return result<void>::make();
        }
        auto& fc = system->fault_controller();
        auto snap = fc.snapshot();

        ctx.output->header("Fault Injection Status");

        std::map<std::string, std::string> kv;
        kv["Enabled"] = snap.enabled ? "yes" : "no";
        kv["Active scope"] = snap.active_scope;
        kv["Replay seed"] = std::to_string(snap.replay_seed);
        kv["Schedule entries"] = std::to_string(snap.schedule_entry_count);
        kv["Faults fired"] = std::to_string(snap.faults_fired);
        ctx.output->key_value(kv);

        return result<void>::make();
    }
};

class FaultListCommand final : public ICommand {
  public:
    std::string_view path() const noexcept override {
        return "fault/list";
    }
    std::string_view help_text() const noexcept override {
        return "List all registered fault injection points";
    }
    int order() const noexcept override {
        return 90;
    }

    result<void> execute(CommandContext& ctx) const override {
        auto& reg = fault::FaultPointRegistry::instance();

        ctx.output->header("Registered Fault Points");
        for (const auto& pt : reg.points()) {
            ctx.output->raw(pt.path + "  [" +
                            std::string(fault::to_string(pt.domain)) + "]  " +
                            pt.description);
        }
        return result<void>::make();
    }
};

class FaultClearCommand final : public ICommand {
  public:
    std::string_view path() const noexcept override {
        return "fault/clear";
    }
    std::string_view help_text() const noexcept override {
        return "Clear fault schedule and disable injection";
    }
    int order() const noexcept override {
        return 90;
    }

    result<void> execute(CommandContext& ctx) const override {
        auto* system = ctx.system;
        if (!system) {
            ctx.output->error("No actor system available");
            return result<void>::make();
        }
        auto& fc = system->fault_controller();
        fc.clear();
        fc.disable();
        ctx.output->raw("Fault schedule cleared, injection disabled");
        return result<void>::make();
    }
};

const CommandRegistration<FaultStatusCommand> kRegisterFaultStatus;
const CommandRegistration<FaultListCommand> kRegisterFaultList;
const CommandRegistration<FaultClearCommand> kRegisterFaultClear;

} // anonymous namespace
} // namespace cli
} // namespace hpactor
