// Copyright 2026 HPActor Contributors
// Licensed under the Apache License, Version 2.0

#include <hpactor/cli/cli_actor.hpp>
#include <hpactor/cli/command_registry.hpp>
#include <hpactor/cli/output_formatter.hpp>

namespace hpactor {
namespace cli {
namespace {

class QuitCommand final : public ICommand {
  public:
    std::string_view path() const noexcept override {
        return "quit";
    }
    std::string_view help_text() const noexcept override {
        return "Exit the CLI";
    }
    int order() const noexcept override {
        return 9999;
    }

    result<void> execute(CommandContext& ctx) const override {
        ctx.output->raw("Goodbye.");
        if (ctx.cli_actor) {
            ctx.cli_actor->request_shutdown();
        }
        return result<void>::make();
    }
};

const CommandRegistration<QuitCommand> kRegisterQuit;

} // anonymous namespace
} // namespace cli
} // namespace hpactor
