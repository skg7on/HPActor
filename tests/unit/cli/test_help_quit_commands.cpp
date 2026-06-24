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
#include <hpactor/cli/format/pretty_formatter.hpp>

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
// HelpCommand
// =============================================================================

TEST(HelpCommandTest, Metadata) {
    auto* cmd = find_cmd("help");
    ASSERT_NE(cmd, nullptr);
    EXPECT_EQ(cmd->help_text(), "Show available commands");
    EXPECT_EQ(cmd->order(), 0);
}

TEST(HelpCommandTest, ExecuteWithNullCliActor) {
    auto* cmd = find_cmd("help");
    ASSERT_NE(cmd, nullptr);

    PrettyFormatter fmt;
    CommandContext ctx;
    ctx.output = &fmt;
    ctx.cli_actor = nullptr;

    cmd->execute(ctx);
    std::string out = fmt.finalize();

    EXPECT_NE(out.find("Available Commands"), std::string::npos);
    // No crash — the null cli_actor branch is exercised.
}

// =============================================================================
// QuitCommand
// =============================================================================

TEST(QuitCommandTest, Metadata) {
    auto* cmd = find_cmd("quit");
    ASSERT_NE(cmd, nullptr);
    EXPECT_EQ(cmd->help_text(), "Exit the CLI");
    EXPECT_EQ(cmd->order(), 9999);
}

TEST(QuitCommandTest, ExecuteWithNullCliActorPrintsGoodbye) {
    auto* cmd = find_cmd("quit");
    ASSERT_NE(cmd, nullptr);

    PrettyFormatter fmt;
    CommandContext ctx;
    ctx.output = &fmt;
    ctx.cli_actor = nullptr;

    cmd->execute(ctx);
    std::string out = fmt.finalize();

    EXPECT_NE(out.find("Goodbye."), std::string::npos);
    // No crash — the null cli_actor guard works.
}
