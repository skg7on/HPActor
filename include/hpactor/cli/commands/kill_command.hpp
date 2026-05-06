// Copyright 2026 HPActor Contributors
// Licensed under the Apache License, Version 2.0
#pragma once

#include <hpactor/cli/command_context.hpp>
#include <hpactor/types/types.hpp>
#include <charconv>

namespace hpactor {
namespace cli {
namespace commands {

inline result<void> execute_kill(CommandContext& ctx) {
    auto id_str = ctx.get_param("<id>");
    if (!id_str) {
        ctx.output->error("Missing actor ID (usage: /actor <id> kill)");
        return result<void>::make();
    }

    uint64_t raw_id = 0;
    auto [ptr, ec] = std::from_chars(id_str->data(), id_str->data() + id_str->size(), raw_id, 0);
    if (ec != std::errc{}) {
        ctx.output->error("Invalid actor ID: " + *id_str);
        return result<void>::make();
    }

    ctx.output->raw("Kill request sent to actor " + *id_str);
    return result<void>::make();
}

}  // namespace commands
}  // namespace cli
}  // namespace hpactor
