// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#include <hpactor/actor/actor_system.hpp>
#include <hpactor/cli/command_registry.hpp>
#include <hpactor/cli/output_formatter.hpp>
#include <hpactor/log/log_config.hpp>
#include <hpactor/log/log_level.hpp>
#include <hpactor/log/log_manager.hpp>

#include <map>
#include <string>

namespace hpactor {
namespace cli {
namespace {

class LogLevelCommand final : public ICommand {
  public:
    std::string_view path() const noexcept override {
        return "log/level";
    }
    std::string_view help_text() const noexcept override {
        return "Show log subsystem status and level";
    }
    int order() const noexcept override {
        return 710;
    }

    result<void> execute(CommandContext& ctx) const override {
        auto* sys = ctx.system;
        if (!sys) {
            ctx.output->error("Internal error: no actor system");
            return result<void>::make();
        }
        auto* lm = sys->log_manager();
        if (!lm) {
            ctx.output->raw("Logging subsystem is not enabled.");
            return result<void>::make();
        }

        ctx.output->header("Log Subsystem Status");
        std::map<std::string, std::string> kv;
        kv["Default level"] =
            std::string(log::to_string(lm->config().default_level));
        kv["Events lost"] = std::to_string(lm->events_lost());
        kv["Sink errors"] = std::to_string(lm->sink_errors());
        ctx.output->key_value(kv);
        return result<void>::make();
    }
};

const CommandRegistration<LogLevelCommand> kRegisterLogLevel;

} // anonymous namespace
} // namespace cli
} // namespace hpactor
