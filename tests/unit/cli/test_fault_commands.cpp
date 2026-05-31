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
#include <hpactor/fault/fault_point.hpp>

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
// FaultListCommand — no ActorSystem needed (uses FaultPointRegistry singleton)
// =============================================================================

TEST(FaultListCommandTest, Metadata) {
    auto* cmd = find_cmd("fault/list");
    ASSERT_NE(cmd, nullptr);
    EXPECT_EQ(cmd->help_text(), "List all registered fault injection points");
}

TEST(FaultListCommandTest, ExecuteEmptyRegistry) {
    auto* cmd = find_cmd("fault/list");
    ASSERT_NE(cmd, nullptr);

    PrettyFormatter fmt;
    CommandContext ctx;
    ctx.output = &fmt;

    cmd->execute(ctx);
    std::string out = fmt.finalize();

    EXPECT_NE(out.find("Registered Fault Points"), std::string::npos);
}

TEST(FaultListCommandTest, ExecuteWithRegisteredPoints) {
    auto& reg = hpactor::fault::FaultPointRegistry::instance();
    reg.register_point("test/point/alpha", hpactor::fault::FaultDomain::kMailbox,
                       "Alpha fault description");
    reg.register_point("test/point/beta", hpactor::fault::FaultDomain::kTransport,
                       "Beta fault description");

    auto* cmd = find_cmd("fault/list");
    ASSERT_NE(cmd, nullptr);

    PrettyFormatter fmt;
    CommandContext ctx;
    ctx.output = &fmt;

    cmd->execute(ctx);
    std::string out = fmt.finalize();

    EXPECT_NE(out.find("Registered Fault Points"), std::string::npos);
    EXPECT_NE(out.find("test/point/alpha"), std::string::npos);
    EXPECT_NE(out.find("Alpha fault description"), std::string::npos);
    EXPECT_NE(out.find("test/point/beta"), std::string::npos);
    EXPECT_NE(out.find("Beta fault description"), std::string::npos);
}

// =============================================================================
// FaultStatusCommand — error paths
// =============================================================================

TEST(FaultStatusCommandTest, Metadata) {
    auto* cmd = find_cmd("fault/status");
    ASSERT_NE(cmd, nullptr);
    EXPECT_EQ(cmd->help_text(), "Show fault injection status");
}

TEST(FaultStatusCommandTest, NullSystem) {
    auto* cmd = find_cmd("fault/status");
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
// FaultClearCommand — error paths
// =============================================================================

TEST(FaultClearCommandTest, Metadata) {
    auto* cmd = find_cmd("fault/clear");
    ASSERT_NE(cmd, nullptr);
    EXPECT_EQ(cmd->help_text(), "Clear fault schedule and disable injection");
}

TEST(FaultClearCommandTest, NullSystem) {
    auto* cmd = find_cmd("fault/clear");
    ASSERT_NE(cmd, nullptr);

    PrettyFormatter fmt;
    CommandContext ctx;
    ctx.output = &fmt;
    ctx.system = nullptr;

    cmd->execute(ctx);
    std::string out = fmt.finalize();

    EXPECT_NE(out.find("No actor system available"), std::string::npos);
}
