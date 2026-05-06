// Copyright 2026 HPActor Contributors
// Licensed under the Apache License, Version 2.0
#pragma once

#include <hpactor/cli/command_context.hpp>
#include <hpactor/types/types.hpp>

namespace hpactor {
namespace cli {
namespace commands {

inline result<void> execute_system_stats(CommandContext& ctx) {
    ctx.output->header("System Statistics");
    ctx.output->raw("stats — not yet implemented");
    return result<void>::make();
}

}  // namespace commands
}  // namespace cli
}  // namespace hpactor
