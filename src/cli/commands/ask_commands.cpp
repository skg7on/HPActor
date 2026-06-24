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

#include <hpactor/cli/command/command_context.hpp>
#include <hpactor/cli/command/command_registry.hpp>
#include <hpactor/cli/format/output_formatter.hpp>
#include <hpactor/types/types.hpp>

namespace hpactor {
namespace cli {
namespace {

class AskPendingCommand final : public ICommand {
  public:
    std::string_view path() const noexcept override {
        return "ask/pending";
    }
    std::string_view help_text() const noexcept override {
        return "List in-flight ask requests";
    }
    int order() const noexcept override {
        return 600;
    }

    result<void> execute(CommandContext& ctx) const override {
        ctx.output->header("Pending Ask Requests");
        ctx.output->raw("Ask manager inspection not yet available.\n");
        return result<void>::make();
    }
};

const CommandRegistration<AskPendingCommand> kRegisterAskPending;

class AskCancelCommand final : public ICommand {
  public:
    std::string_view path() const noexcept override {
        return "ask/cancel";
    }
    std::string_view help_text() const noexcept override {
        return "Cancel an ask request by message ID";
    }
    int order() const noexcept override {
        return 601;
    }

    result<void> execute(CommandContext& ctx) const override {
        ctx.output->header("Cancel Ask Request");
        ctx.output->raw("Ask manager inspection not yet available.\n");
        return result<void>::make();
    }
};

const CommandRegistration<AskCancelCommand> kRegisterAskCancel;

class AskStatsCommand final : public ICommand {
  public:
    std::string_view path() const noexcept override {
        return "ask/stats";
    }
    std::string_view help_text() const noexcept override {
        return "Ask manager statistics";
    }
    int order() const noexcept override {
        return 602;
    }

    result<void> execute(CommandContext& ctx) const override {
        ctx.output->header("Ask Manager Statistics");
        ctx.output->raw("Ask manager inspection not yet available.\n");
        return result<void>::make();
    }
};

const CommandRegistration<AskStatsCommand> kRegisterAskStats;

} // anonymous namespace
} // namespace cli
} // namespace hpactor
