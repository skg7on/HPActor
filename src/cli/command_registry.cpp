// Copyright 2026 HPActor Contributors
// Licensed under the Apache License, Version 2.0

#include <hpactor/cli/command_registry.hpp>

namespace hpactor {
namespace cli {

CommandRegistry& CommandRegistry::instance() {
    static CommandRegistry reg;
    return reg;
}

void CommandRegistry::add(std::unique_ptr<ICommand> cmd) {
    commands_.push_back(std::move(cmd));
}

const std::vector<std::unique_ptr<ICommand>>& CommandRegistry::commands() const {
    return commands_;
}

} // namespace cli
} // namespace hpactor
