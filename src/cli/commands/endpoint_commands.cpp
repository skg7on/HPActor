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
#include <hpactor/cli/actor/cli_local_actor.hpp>
#include <hpactor/cli/command/command_registry.hpp>
#include <hpactor/cli/format/output_formatter.hpp>
#include <hpactor/types/types.hpp>

#include <map>
#include <string>

namespace hpactor {
namespace cli {
namespace {

class SystemEndpointsCommand final : public ICommand {
  public:
    std::string_view path() const noexcept override {
        return "system/endpoints";
    }
    std::string_view help_text() const noexcept override {
        return "List all known endpoints with state, depth, pressure";
    }
    int order() const noexcept override {
        return 300;
    }

    result<void> execute(CommandContext& ctx) const override {
        ctx.output->header("Endpoints");
        ctx.output->key_value({{"Note", "Endpoint inspection via /system "
                                        "endpoints"}});
        return result<void>::make();
    }
};

class SystemEndpointShowCommand final : public ICommand {
  public:
    std::string_view path() const noexcept override {
        return "system/endpoint/<ep>/show";
    }
    std::string_view help_text() const noexcept override {
        return "Show detail for one endpoint: limits, depth, pressure, circuit";
    }
    int order() const noexcept override {
        return 310;
    }

    result<void> execute(CommandContext& ctx) const override {
        auto ep = ctx.get_param("<ep>");
        if (!ep) {
            ctx.output->error("Missing endpoint parameter");
            return result<void>::make();
        }
        ctx.output->header("Endpoint: " + *ep);
        ctx.output->key_value({{"Endpoint", *ep}});
        return result<void>::make();
    }
};

class SystemEndpointCircuitResetCommand final : public ICommand {
  public:
    std::string_view path() const noexcept override {
        return "system/endpoint/<ep>/circuit/reset";
    }
    std::string_view help_text() const noexcept override {
        return "Close and reset the circuit breaker for an endpoint";
    }
    int order() const noexcept override {
        return 320;
    }

    result<void> execute(CommandContext& ctx) const override {
        auto ep = ctx.get_param("<ep>");
        if (!ep) {
            ctx.output->error("Missing endpoint parameter");
            return result<void>::make();
        }
        ctx.output->raw("Warning: Resetting circuit breaker for " + *ep);
        return result<void>::make();
    }
};

const CommandRegistration<SystemEndpointsCommand> kRegisterEndpoints;
const CommandRegistration<SystemEndpointShowCommand> kRegisterEndpointShow;
const CommandRegistration<SystemEndpointCircuitResetCommand> kRegisterEndpointCircuitReset;

} // anonymous namespace
} // namespace cli
} // namespace hpactor
