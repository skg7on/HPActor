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

#include <hpactor/cli/command_context.hpp>
#include <hpactor/cli/command_registry.hpp>
#include <hpactor/cli/output_formatter.hpp>
#include <hpactor/cli/pretty_formatter.hpp>

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
// DlqListCommand — unit tests (error paths, no ActorSystem)
// =============================================================================

TEST(DlqListCommandTest, Metadata) {
    auto* cmd = find_cmd("dlq/list");
    ASSERT_NE(cmd, nullptr);
    EXPECT_EQ(cmd->help_text(), "List dead-letter queue records");
    EXPECT_EQ(cmd->order(), 500);
}

TEST(DlqListCommandTest, NullSystem) {
    auto* cmd = find_cmd("dlq/list");
    ASSERT_NE(cmd, nullptr);

    PrettyFormatter fmt;
    CommandContext ctx;
    ctx.output = &fmt;
    ctx.system = nullptr;

    cmd->execute(ctx);
    std::string out = fmt.finalize();

    EXPECT_NE(out.find("No actor system available"), std::string::npos);
}

// =============================================================================
// DlqShowCommand — unit tests (error paths, no ActorSystem)
// =============================================================================

TEST(DlqShowCommandTest, Metadata) {
    auto* cmd = find_cmd("dlq/show");
    ASSERT_NE(cmd, nullptr);
    EXPECT_EQ(cmd->help_text(), "Show a dead-letter record: /dlq show --index "
                                "N");
    EXPECT_EQ(cmd->order(), 510);
}

TEST(DlqShowCommandTest, NullSystem) {
    auto* cmd = find_cmd("dlq/show");
    ASSERT_NE(cmd, nullptr);

    PrettyFormatter fmt;
    CommandContext ctx;
    ctx.output = &fmt;
    ctx.system = nullptr;

    cmd->execute(ctx);
    std::string out = fmt.finalize();

    EXPECT_NE(out.find("No actor system available"), std::string::npos);
}

// =============================================================================
// DlqReplayCommand — unit tests (error paths, no ActorSystem)
// =============================================================================

TEST(DlqReplayCommandTest, Metadata) {
    auto* cmd = find_cmd("dlq/replay");
    ASSERT_NE(cmd, nullptr);
    EXPECT_EQ(cmd->help_text(), "Replay a dead-letter record: /dlq replay "
                                "--index N");
    EXPECT_EQ(cmd->order(), 520);
}

TEST(DlqReplayCommandTest, NullSystem) {
    auto* cmd = find_cmd("dlq/replay");
    ASSERT_NE(cmd, nullptr);

    PrettyFormatter fmt;
    CommandContext ctx;
    ctx.output = &fmt;
    ctx.system = nullptr;

    cmd->execute(ctx);
    std::string out = fmt.finalize();

    EXPECT_NE(out.find("No actor system available"), std::string::npos);
}

// =============================================================================
// DlqExportCommand — unit tests (error paths, no ActorSystem)
// =============================================================================

TEST(DlqExportCommandTest, Metadata) {
    auto* cmd = find_cmd("dlq/export");
    ASSERT_NE(cmd, nullptr);
    EXPECT_EQ(cmd->help_text(), "Export dead-letter records: /dlq export "
                                "[--format json|text]");
    EXPECT_EQ(cmd->order(), 530);
}

TEST(DlqExportCommandTest, NullSystem) {
    auto* cmd = find_cmd("dlq/export");
    ASSERT_NE(cmd, nullptr);

    PrettyFormatter fmt;
    CommandContext ctx;
    ctx.output = &fmt;
    ctx.system = nullptr;

    cmd->execute(ctx);
    std::string out = fmt.finalize();

    EXPECT_NE(out.find("No actor system available"), std::string::npos);
}
