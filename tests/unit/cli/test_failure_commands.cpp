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
#include <hpactor/msg/failure_reason.hpp>

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

std::string execute_cmd(ICommand& cmd) {
    PrettyFormatter fmt;
    CommandContext ctx;
    ctx.output = &fmt;
    cmd.execute(ctx);
    return fmt.finalize();
}

} // anonymous namespace

// =============================================================================
// FailureReasonsCommand
// =============================================================================

TEST(FailureReasonsCommandTest, Metadata) {
    auto* cmd = find_cmd("failure/reasons");
    ASSERT_NE(cmd, nullptr);
    EXPECT_EQ(cmd->help_text(), "List all canonical failure reasons");
    EXPECT_EQ(cmd->order(), 600);
}

TEST(FailureReasonsCommandTest, ExecuteProducesTable) {
    auto* cmd = find_cmd("failure/reasons");
    ASSERT_NE(cmd, nullptr);

    std::string out = execute_cmd(*cmd);

    EXPECT_NE(out.find("Canonical Failure Reasons"), std::string::npos);
    EXPECT_NE(out.find("Reason"), std::string::npos);
    EXPECT_NE(out.find("Code"), std::string::npos);
    EXPECT_NE(out.find("Retryable"), std::string::npos);
}

TEST(FailureReasonsCommandTest, ExecuteContainsAllReasonStrings) {
    auto* cmd = find_cmd("failure/reasons");
    ASSERT_NE(cmd, nullptr);

    std::string out = execute_cmd(*cmd);

    // Spot-check a selection of failure reason strings (snake_case from
    // to_string)
    EXPECT_NE(out.find("no_route"), std::string::npos);
    EXPECT_NE(out.find("actor_dead"), std::string::npos);
    EXPECT_NE(out.find("mailbox_full"), std::string::npos);
    EXPECT_NE(out.find("expired"), std::string::npos);
    EXPECT_NE(out.find("unknown"), std::string::npos);
}

TEST(FailureReasonsCommandTest, ExecuteContainsRetryableFlags) {
    auto* cmd = find_cmd("failure/reasons");
    ASSERT_NE(cmd, nullptr);

    std::string out = execute_cmd(*cmd);

    // Some reasons are retryable, some are not
    EXPECT_NE(out.find("yes"), std::string::npos);
    EXPECT_NE(out.find("no"), std::string::npos);
}

// =============================================================================
// FailureSummaryCommand
// =============================================================================

TEST(FailureSummaryCommandTest, Metadata) {
    auto* cmd = find_cmd("failure/summary");
    ASSERT_NE(cmd, nullptr);
    EXPECT_EQ(cmd->help_text(), "Show failure subsystem status");
    EXPECT_EQ(cmd->order(), 610);
}

TEST(FailureSummaryCommandTest, ExecuteProducesKeyValueOutput) {
    auto* cmd = find_cmd("failure/summary");
    ASSERT_NE(cmd, nullptr);

    std::string out = execute_cmd(*cmd);

    EXPECT_NE(out.find("Failure Subsystem Status"), std::string::npos);
    EXPECT_NE(out.find("FailureReason values"), std::string::npos);
    EXPECT_NE(out.find("FailureSource values"), std::string::npos);
    EXPECT_NE(out.find("DLQ mapping"), std::string::npos);
    EXPECT_NE(out.find("Spawn mapping"), std::string::npos);
    EXPECT_NE(out.find("EnqueueResultCode mapping"), std::string::npos);
    EXPECT_NE(out.find("Phase"), std::string::npos);
}

TEST(FailureSummaryCommandTest, ExecuteContainsHintLines) {
    auto* cmd = find_cmd("failure/summary");
    ASSERT_NE(cmd, nullptr);

    std::string out = execute_cmd(*cmd);

    EXPECT_NE(out.find("/failure reasons"), std::string::npos);
    EXPECT_NE(out.find("/actor"), std::string::npos);
}
