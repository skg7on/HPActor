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

#include <hpactor/cli/cli_local_actor.hpp>
#include <hpactor/cli/command_context.hpp>
#include <hpactor/cli/command_registry.hpp>
#include <hpactor/cli/pretty_formatter.hpp>
#include <hpactor/mem/memory_config.hpp>

#include <gtest/gtest.h>

using namespace hpactor::cli;

namespace {

ICommand* find_cmd(std::string_view path) {
    auto& reg = CommandRegistry::instance();
    for (auto& c : reg.commands()) {
        if (c->path() == path)
            return c.get();
    }
    return nullptr;
}

} // anonymous namespace

// =============================================================================
// SystemStatsCommand
// =============================================================================

TEST(SystemStatsCommandTest, Metadata) {
    auto* cmd = find_cmd("system/stats");
    ASSERT_NE(cmd, nullptr);
    EXPECT_EQ(cmd->help_text(), "System-wide statistics");
    EXPECT_EQ(cmd->order(), 100);
}

TEST(SystemStatsCommandTest, NullSystem) {
    auto* cmd = find_cmd("system/stats");
    ASSERT_NE(cmd, nullptr);

    PrettyFormatter fmt;
    CommandContext ctx;
    ctx.output = &fmt;
    ctx.system = nullptr;

    cmd->execute(ctx);
    std::string out = fmt.finalize();

    EXPECT_NE(out.find("Internal error: no actor system"), std::string::npos);
}

// =============================================================================
// SystemMemoryCommand
// =============================================================================

TEST(SystemMemoryCommandTest, Metadata) {
    auto* cmd = find_cmd("system/memory");
    ASSERT_NE(cmd, nullptr);
    EXPECT_EQ(cmd->help_text(), "Memory subsystem stats");
    EXPECT_EQ(cmd->order(), 200);
}

TEST(SystemMemoryCommandTest, Execute) {
    auto* cmd = find_cmd("system/memory");
    ASSERT_NE(cmd, nullptr);

    PrettyFormatter fmt;
    CommandContext ctx;
    ctx.output = &fmt;

    cmd->execute(ctx);
    std::string out = fmt.finalize();

    EXPECT_NE(out.find("Memory Regions"), std::string::npos);
    EXPECT_NE(out.find("Region"), std::string::npos);
    EXPECT_NE(out.find("Active"), std::string::npos);
    EXPECT_NE(out.find("Actor"), std::string::npos);
    EXPECT_NE(out.find("Message"), std::string::npos);
}

// =============================================================================
// SystemListCommand
// =============================================================================

TEST(SystemListCommandTest, Metadata) {
    auto* cmd = find_cmd("system/list");
    ASSERT_NE(cmd, nullptr);
    EXPECT_EQ(cmd->help_text(), "List system actors");
    EXPECT_EQ(cmd->order(), 300);
}

TEST(SystemListCommandTest, NullSystem) {
    auto* cmd = find_cmd("system/list");
    ASSERT_NE(cmd, nullptr);

    PrettyFormatter fmt;
    CommandContext ctx;
    ctx.output = &fmt;
    ctx.system = nullptr;

    cmd->execute(ctx);
    std::string out = fmt.finalize();

    EXPECT_NE(out.find("Internal error: no actor system"), std::string::npos);
}

// =============================================================================
// SystemDrainCommand
// =============================================================================

TEST(SystemDrainCommandTest, Metadata) {
    auto* cmd = find_cmd("system/drain");
    ASSERT_NE(cmd, nullptr);
    EXPECT_EQ(cmd->help_text(), "Graceful node shutdown");
    EXPECT_EQ(cmd->order(), 400);
}

TEST(SystemDrainCommandTest, NullSystem) {
    auto* cmd = find_cmd("system/drain");
    ASSERT_NE(cmd, nullptr);

    PrettyFormatter fmt;
    CommandContext ctx;
    ctx.output = &fmt;
    ctx.system = nullptr;

    cmd->execute(ctx);
    std::string out = fmt.finalize();

    EXPECT_NE(out.find("Internal error: no actor system"), std::string::npos);
}

// =============================================================================
// SystemDrainStatusCommand
// =============================================================================

TEST(SystemDrainStatusCommandTest, Metadata) {
    auto* cmd = find_cmd("system/drain/status");
    ASSERT_NE(cmd, nullptr);
    EXPECT_EQ(cmd->help_text(), "Show shutdown progress");
    EXPECT_EQ(cmd->order(), 410);
}

TEST(SystemDrainStatusCommandTest, NullSystem) {
    auto* cmd = find_cmd("system/drain/status");
    ASSERT_NE(cmd, nullptr);

    PrettyFormatter fmt;
    CommandContext ctx;
    ctx.output = &fmt;
    ctx.system = nullptr;

    cmd->execute(ctx);
    std::string out = fmt.finalize();

    EXPECT_NE(out.find("Internal error: no actor system"), std::string::npos);
}

// =============================================================================
// SystemStopCommand
// =============================================================================

TEST(SystemStopCommandTest, Metadata) {
    auto* cmd = find_cmd("system/stop/<actor_id>");
    ASSERT_NE(cmd, nullptr);
    EXPECT_EQ(cmd->help_text(), "Graceful stop of an actor [--force]");
    EXPECT_EQ(cmd->order(), 500);
}

TEST(SystemStopCommandTest, MissingActorId) {
    auto* cmd = find_cmd("system/stop/<actor_id>");
    ASSERT_NE(cmd, nullptr);

    PrettyFormatter fmt;
    CommandContext ctx;
    ctx.output = &fmt;

    cmd->execute(ctx);
    std::string out = fmt.finalize();

    EXPECT_NE(out.find("Missing actor ID"), std::string::npos);
}

TEST(SystemStopCommandTest, InvalidActorId) {
    auto* cmd = find_cmd("system/stop/<actor_id>");
    ASSERT_NE(cmd, nullptr);

    PrettyFormatter fmt;
    CommandContext ctx;
    ctx.output = &fmt;
    ctx.params["<actor_id>"] = "bogus";

    cmd->execute(ctx);
    std::string out = fmt.finalize();

    EXPECT_NE(out.find("Invalid actor ID"), std::string::npos);
}

TEST(SystemStopCommandTest, NullSystem) {
    auto* cmd = find_cmd("system/stop/<actor_id>");
    ASSERT_NE(cmd, nullptr);

    PrettyFormatter fmt;
    CommandContext ctx;
    ctx.output = &fmt;
    ctx.params["<actor_id>"] = "0x9999";
    ctx.system = nullptr;
    ctx.cli_actor = nullptr; // both null

    cmd->execute(ctx);
    std::string out = fmt.finalize();

    EXPECT_NE(out.find("Internal error"), std::string::npos);
}

TEST(SystemStopCommandTest, ForceFlagParsed) {
    auto* cmd = find_cmd("system/stop/<actor_id>");
    ASSERT_NE(cmd, nullptr);

    PrettyFormatter fmt;
    CommandContext ctx;
    ctx.output = &fmt;
    ctx.params["<actor_id>"] = "0x9999";
    ctx.params["force"] = "true"; // --force flag set
    ctx.system = nullptr;         // still null, but force param is present

    cmd->execute(ctx);
    std::string out = fmt.finalize();

    EXPECT_NE(out.find("Internal error"), std::string::npos);
}

// =============================================================================
// SystemMemoryCommand — per-actor memory
// =============================================================================

TEST(SystemMemoryCommandTest, PerActorMemoryColumns) {
    auto* cmd = find_cmd("system/memory");
    ASSERT_NE(cmd, nullptr);

    PrettyFormatter fmt;
    CommandContext ctx;
    ctx.output = &fmt;

    cmd->execute(ctx);
    std::string out = fmt.finalize();

    // The output should now include per-actor memory section
    // when memory tracking is enabled.
    if constexpr (hpactor::mem::kMemoryTrackingEnabled) {
        EXPECT_NE(out.find("Per-Actor Memory"), std::string::npos);
        EXPECT_NE(out.find("Current"), std::string::npos);
        EXPECT_NE(out.find("Peak"), std::string::npos);
    } else {
        EXPECT_NE(out.find("disabled"), std::string::npos);
    }
}

// =============================================================================
// CliActor kActorTypeName
// =============================================================================

TEST(CliActorTest, HasActorTypeName) {
    EXPECT_STREQ(hpactor::cli::CliActor::kActorTypeName, "CliActor");
}
