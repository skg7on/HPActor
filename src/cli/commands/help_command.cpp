// Copyright 2026 HPActor Contributors
// Licensed under the Apache License, Version 2.0

#include <hpactor/cli/cli_actor.hpp>
#include <hpactor/cli/command_registry.hpp>
#include <hpactor/cli/output_formatter.hpp>

namespace hpactor {
namespace cli {
namespace {

class HelpCommand final : public ICommand {
  public:
    std::string_view path() const noexcept override {
        return "help";
    }
    std::string_view help_text() const noexcept override {
        return "Show available commands";
    }
    int order() const noexcept override {
        return 0;
    }

    result<void> execute(CommandContext& ctx) const override {
        ctx.output->header("Available Commands");
        if (ctx.cli_actor && ctx.cli_actor->command_tree()) {
            ctx.output->raw(ctx.cli_actor->command_tree()->help());
        }
        return result<void>::make();
    }
};

const CommandRegistration<HelpCommand> kRegisterHelp;

} // anonymous namespace
} // namespace cli
} // namespace hpactor
