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

std::string execute_cmd(ICommand& cmd) {
    PrettyFormatter fmt;
    CommandContext ctx;
    ctx.output = &fmt;
    cmd.execute(ctx);
    return fmt.finalize();
}

} // anonymous namespace

// =============================================================================
// MetricsShowCommand
// =============================================================================

TEST(MetricsShowCommandTest, Metadata) {
    auto* cmd = find_cmd("metrics/show");
    ASSERT_NE(cmd, nullptr);
    EXPECT_EQ(cmd->help_text(), "Show current metrics snapshot");
    EXPECT_EQ(cmd->order(), 100);
}

TEST(MetricsShowCommandTest, ExecuteShowsNotImplemented) {
    auto* cmd = find_cmd("metrics/show");
    ASSERT_NE(cmd, nullptr);

    std::string out = execute_cmd(*cmd);

    EXPECT_NE(out.find("Metrics"), std::string::npos);
    EXPECT_NE(out.find("not yet implemented"), std::string::npos);
}

// =============================================================================
// TopologyShowCommand
// =============================================================================

TEST(TopologyShowCommandTest, Metadata) {
    auto* cmd = find_cmd("topology/show");
    ASSERT_NE(cmd, nullptr);
    EXPECT_EQ(cmd->help_text(), "Show topology tree");
    EXPECT_EQ(cmd->order(), 100);
}

TEST(TopologyShowCommandTest, ExecuteShowsNotImplemented) {
    auto* cmd = find_cmd("topology/show");
    ASSERT_NE(cmd, nullptr);

    std::string out = execute_cmd(*cmd);

    EXPECT_NE(out.find("Topology"), std::string::npos);
    EXPECT_NE(out.find("not yet implemented"), std::string::npos);
}
