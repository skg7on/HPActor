// Copyright 2026 HPActor Contributors
// Licensed under the Apache License, Version 2.0
#pragma once

#include <hpactor/cli/command_context.hpp>
#include <hpactor/types/types.hpp>

namespace hpactor {
namespace cli {
namespace commands {

inline result<void> execute_system_memory(CommandContext& ctx) {
    ctx.output->header("System Memory");
    ctx.output->raw("memory — not yet implemented");
    return result<void>::make();
}

}  // namespace commands
}  // namespace cli
}  // namespace hpactor
