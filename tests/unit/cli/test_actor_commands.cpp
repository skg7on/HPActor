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

} // anonymous namespace

// =============================================================================
// ActorShowCommand
// =============================================================================

TEST(ActorShowCommandTest, Metadata) {
    auto* cmd = find_cmd("actor/<id>/show");
    ASSERT_NE(cmd, nullptr);
    EXPECT_EQ(cmd->help_text(), "Display actor metadata, state, mailbox, and "
                                "children");
    EXPECT_EQ(cmd->order(), 100);
}

TEST(ActorShowCommandTest, MissingActorId) {
    auto* cmd = find_cmd("actor/<id>/show");
    ASSERT_NE(cmd, nullptr);

    PrettyFormatter fmt;
    CommandContext ctx;
    ctx.output = &fmt;
    // Do not set params["<id>"] — simulates missing parameter

    cmd->execute(ctx);
    std::string out = fmt.finalize();

    EXPECT_NE(out.find("Missing actor ID"), std::string::npos);
}

TEST(ActorShowCommandTest, InvalidActorId) {
    auto* cmd = find_cmd("actor/<id>/show");
    ASSERT_NE(cmd, nullptr);

    PrettyFormatter fmt;
    CommandContext ctx;
    ctx.output = &fmt;
    ctx.params["<id>"] = "garbage";

    cmd->execute(ctx);
    std::string out = fmt.finalize();

    EXPECT_NE(out.find("Invalid actor ID"), std::string::npos);
}

TEST(ActorShowCommandTest, InvalidActorIdZero) {
    auto* cmd = find_cmd("actor/<id>/show");
    ASSERT_NE(cmd, nullptr);

    PrettyFormatter fmt;
    CommandContext ctx;
    ctx.output = &fmt;
    ctx.params["<id>"] = "0"; // parse_actor_id("0") returns ActorId{0}

    cmd->execute(ctx);
    std::string out = fmt.finalize();

    EXPECT_NE(out.find("Invalid actor ID"), std::string::npos);
}

TEST(ActorShowCommandTest, NullCliActor) {
    auto* cmd = find_cmd("actor/<id>/show");
    ASSERT_NE(cmd, nullptr);

    PrettyFormatter fmt;
    CommandContext ctx;
    ctx.output = &fmt;
    ctx.params["<id>"] = "0x1234";
    ctx.cli_actor = nullptr;

    cmd->execute(ctx);
    std::string out = fmt.finalize();

    EXPECT_NE(out.find("Internal error: no CLI actor"), std::string::npos);
}

// =============================================================================
// ActorKillCommand
// =============================================================================

TEST(ActorKillCommandTest, Metadata) {
    auto* cmd = find_cmd("actor/<id>/kill");
    ASSERT_NE(cmd, nullptr);
    EXPECT_EQ(cmd->help_text(), "Terminate actor (graceful shutdown)");
    EXPECT_EQ(cmd->order(), 200);
}

TEST(ActorKillCommandTest, MissingActorId) {
    auto* cmd = find_cmd("actor/<id>/kill");
    ASSERT_NE(cmd, nullptr);

    PrettyFormatter fmt;
    CommandContext ctx;
    ctx.output = &fmt;

    cmd->execute(ctx);
    std::string out = fmt.finalize();

    EXPECT_NE(out.find("Missing actor ID"), std::string::npos);
}

TEST(ActorKillCommandTest, InvalidActorId) {
    auto* cmd = find_cmd("actor/<id>/kill");
    ASSERT_NE(cmd, nullptr);

    PrettyFormatter fmt;
    CommandContext ctx;
    ctx.output = &fmt;
    ctx.params["<id>"] = "garbage";

    cmd->execute(ctx);
    std::string out = fmt.finalize();

    EXPECT_NE(out.find("Invalid actor ID"), std::string::npos);
}

TEST(ActorKillCommandTest, NullCliActor) {
    auto* cmd = find_cmd("actor/<id>/kill");
    ASSERT_NE(cmd, nullptr);

    PrettyFormatter fmt;
    CommandContext ctx;
    ctx.output = &fmt;
    ctx.params["<id>"] = "0x5678";
    ctx.cli_actor = nullptr;

    cmd->execute(ctx);
    std::string out = fmt.finalize();

    EXPECT_NE(out.find("Internal error: no CLI actor"), std::string::npos);
}

// =============================================================================
// ActorListCommand
// =============================================================================

TEST(ActorListCommandTest, Metadata) {
    auto* cmd = find_cmd("actor/list");
    ASSERT_NE(cmd, nullptr);
    EXPECT_EQ(cmd->help_text(), "List all actors [--filter <type>]");
    EXPECT_EQ(cmd->order(), 300);
}

TEST(ActorListCommandTest, NullCliActor) {
    auto* cmd = find_cmd("actor/list");
    ASSERT_NE(cmd, nullptr);

    PrettyFormatter fmt;
    CommandContext ctx;
    ctx.output = &fmt;
    ctx.cli_actor = nullptr;

    cmd->execute(ctx);
    std::string out = fmt.finalize();

    EXPECT_NE(out.find("Internal error: no CLI actor"), std::string::npos);
}

TEST(ActorListCommandTest, FilterParamParsedWhenSet) {
    auto* cmd = find_cmd("actor/list");
    ASSERT_NE(cmd, nullptr);

    PrettyFormatter fmt;
    CommandContext ctx;
    ctx.output = &fmt;
    ctx.params["filter"] = "Worker";
    ctx.cli_actor = nullptr; // still null, but filter parsing happens before
                             // null check

    cmd->execute(ctx);
    std::string out = fmt.finalize();

    EXPECT_NE(out.find("Internal error: no CLI actor"), std::string::npos);
}

// =============================================================================
// ActorCircuitCommand
// =============================================================================

TEST(ActorCircuitCommandTest, Metadata) {
    auto* cmd = find_cmd("actor/<id>/circuit");
    ASSERT_NE(cmd, nullptr);
    EXPECT_EQ(cmd->help_text(), "Show circuit breaker state: state, trip "
                                "count, failure EMA");
    EXPECT_EQ(cmd->order(), 150);
}

TEST(ActorCircuitCommandTest, MissingActorId) {
    auto* cmd = find_cmd("actor/<id>/circuit");
    ASSERT_NE(cmd, nullptr);

    PrettyFormatter fmt;
    CommandContext ctx;
    ctx.output = &fmt;

    cmd->execute(ctx);
    std::string out = fmt.finalize();

    EXPECT_NE(out.find("Missing actor ID"), std::string::npos);
}

TEST(ActorCircuitCommandTest, InvalidActorId) {
    auto* cmd = find_cmd("actor/<id>/circuit");
    ASSERT_NE(cmd, nullptr);

    PrettyFormatter fmt;
    CommandContext ctx;
    ctx.output = &fmt;
    ctx.params["<id>"] = "garbage";

    cmd->execute(ctx);
    std::string out = fmt.finalize();

    EXPECT_NE(out.find("Invalid actor ID"), std::string::npos);
}

TEST(ActorCircuitCommandTest, NullCliActor) {
    auto* cmd = find_cmd("actor/<id>/circuit");
    ASSERT_NE(cmd, nullptr);

    PrettyFormatter fmt;
    CommandContext ctx;
    ctx.output = &fmt;
    ctx.params["<id>"] = "0x1234";
    ctx.cli_actor = nullptr;

    cmd->execute(ctx);
    std::string out = fmt.finalize();

    EXPECT_NE(out.find("Internal error: no CLI actor"), std::string::npos);
}

// =============================================================================
// ActorQuarantineCommand
// =============================================================================

TEST(ActorQuarantineCommandTest, Metadata) {
    auto* cmd = find_cmd("actor/<id>/quarantine");
    ASSERT_NE(cmd, nullptr);
    EXPECT_EQ(cmd->help_text(), "Manually quarantine an actor [--reason "
                                "<text>]");
    EXPECT_EQ(cmd->order(), 250);
}

TEST(ActorQuarantineCommandTest, MissingActorId) {
    auto* cmd = find_cmd("actor/<id>/quarantine");
    ASSERT_NE(cmd, nullptr);

    PrettyFormatter fmt;
    CommandContext ctx;
    ctx.output = &fmt;

    cmd->execute(ctx);
    std::string out = fmt.finalize();

    EXPECT_NE(out.find("Missing actor ID"), std::string::npos);
}

TEST(ActorQuarantineCommandTest, InvalidActorId) {
    auto* cmd = find_cmd("actor/<id>/quarantine");
    ASSERT_NE(cmd, nullptr);

    PrettyFormatter fmt;
    CommandContext ctx;
    ctx.output = &fmt;
    ctx.params["<id>"] = "garbage";

    cmd->execute(ctx);
    std::string out = fmt.finalize();

    EXPECT_NE(out.find("Invalid actor ID"), std::string::npos);
}

TEST(ActorQuarantineCommandTest, NullCliActor) {
    auto* cmd = find_cmd("actor/<id>/quarantine");
    ASSERT_NE(cmd, nullptr);

    PrettyFormatter fmt;
    CommandContext ctx;
    ctx.output = &fmt;
    ctx.params["<id>"] = "0x1234";
    ctx.cli_actor = nullptr;

    cmd->execute(ctx);
    std::string out = fmt.finalize();

    EXPECT_NE(out.find("Internal error: no CLI actor"), std::string::npos);
}

// =============================================================================
// ActorUnquarantineCommand
// =============================================================================

TEST(ActorUnquarantineCommandTest, Metadata) {
    auto* cmd = find_cmd("actor/<id>/unquarantine");
    ASSERT_NE(cmd, nullptr);
    EXPECT_EQ(cmd->help_text(), "Release an actor from quarantine");
    EXPECT_EQ(cmd->order(), 260);
}

TEST(ActorUnquarantineCommandTest, MissingActorId) {
    auto* cmd = find_cmd("actor/<id>/unquarantine");
    ASSERT_NE(cmd, nullptr);

    PrettyFormatter fmt;
    CommandContext ctx;
    ctx.output = &fmt;

    cmd->execute(ctx);
    std::string out = fmt.finalize();

    EXPECT_NE(out.find("Missing actor ID"), std::string::npos);
}

TEST(ActorUnquarantineCommandTest, InvalidActorId) {
    auto* cmd = find_cmd("actor/<id>/unquarantine");
    ASSERT_NE(cmd, nullptr);

    PrettyFormatter fmt;
    CommandContext ctx;
    ctx.output = &fmt;
    ctx.params["<id>"] = "garbage";

    cmd->execute(ctx);
    std::string out = fmt.finalize();

    EXPECT_NE(out.find("Invalid actor ID"), std::string::npos);
}

TEST(ActorUnquarantineCommandTest, NullCliActor) {
    auto* cmd = find_cmd("actor/<id>/unquarantine");
    ASSERT_NE(cmd, nullptr);

    PrettyFormatter fmt;
    CommandContext ctx;
    ctx.output = &fmt;
    ctx.params["<id>"] = "0x5678";
    ctx.cli_actor = nullptr;

    cmd->execute(ctx);
    std::string out = fmt.finalize();

    EXPECT_NE(out.find("Internal error: no CLI actor"), std::string::npos);
}

// =============================================================================
// ActorDeliveryCommand
// =============================================================================

TEST(ActorDeliveryCommandTest, Metadata) {
    auto* cmd = find_cmd("actor/<id>/delivery");
    ASSERT_NE(cmd, nullptr);
    EXPECT_EQ(cmd->help_text(), "Show per-actor delivery result counters");
    EXPECT_EQ(cmd->order(), 285);
}

TEST(ActorDeliveryCommandTest, MissingActorId) {
    auto* cmd = find_cmd("actor/<id>/delivery");
    ASSERT_NE(cmd, nullptr);

    PrettyFormatter fmt;
    CommandContext ctx;
    ctx.output = &fmt;

    cmd->execute(ctx);
    std::string out = fmt.finalize();

    EXPECT_NE(out.find("Missing actor ID"), std::string::npos);
}

TEST(ActorDeliveryCommandTest, InvalidActorId) {
    auto* cmd = find_cmd("actor/<id>/delivery");
    ASSERT_NE(cmd, nullptr);

    PrettyFormatter fmt;
    CommandContext ctx;
    ctx.output = &fmt;
    ctx.params["<id>"] = "garbage";

    cmd->execute(ctx);
    std::string out = fmt.finalize();

    EXPECT_NE(out.find("Invalid actor ID"), std::string::npos);
}

TEST(ActorDeliveryCommandTest, NullCliActor) {
    auto* cmd = find_cmd("actor/<id>/delivery");
    ASSERT_NE(cmd, nullptr);

    PrettyFormatter fmt;
    CommandContext ctx;
    ctx.output = &fmt;
    ctx.params["<id>"] = "0xABCD";
    ctx.cli_actor = nullptr;

    cmd->execute(ctx);
    std::string out = fmt.finalize();

    EXPECT_NE(out.find("Internal error: no CLI actor"), std::string::npos);
}

// =============================================================================
// ActorDeliveryStatsCommand
// =============================================================================

TEST(ActorDeliveryStatsCommandTest, Metadata) {
    auto* cmd = find_cmd("actor/<id>/delivery-stats");
    ASSERT_NE(cmd, nullptr);
    EXPECT_EQ(cmd->help_text(), "Show delivery statistics with accept/reject/"
                                "retry ratios");
    EXPECT_EQ(cmd->order(), 286);
}

TEST(ActorDeliveryStatsCommandTest, MissingActorId) {
    auto* cmd = find_cmd("actor/<id>/delivery-stats");
    ASSERT_NE(cmd, nullptr);

    PrettyFormatter fmt;
    CommandContext ctx;
    ctx.output = &fmt;

    cmd->execute(ctx);
    std::string out = fmt.finalize();

    EXPECT_NE(out.find("Missing actor ID"), std::string::npos);
}

TEST(ActorDeliveryStatsCommandTest, InvalidActorId) {
    auto* cmd = find_cmd("actor/<id>/delivery-stats");
    ASSERT_NE(cmd, nullptr);

    PrettyFormatter fmt;
    CommandContext ctx;
    ctx.output = &fmt;
    ctx.params["<id>"] = "garbage";

    cmd->execute(ctx);
    std::string out = fmt.finalize();

    EXPECT_NE(out.find("Invalid actor ID"), std::string::npos);
}

TEST(ActorDeliveryStatsCommandTest, NullCliActor) {
    auto* cmd = find_cmd("actor/<id>/delivery-stats");
    ASSERT_NE(cmd, nullptr);

    PrettyFormatter fmt;
    CommandContext ctx;
    ctx.output = &fmt;
    ctx.params["<id>"] = "0xBEEF";
    ctx.cli_actor = nullptr;

    cmd->execute(ctx);
    std::string out = fmt.finalize();

    EXPECT_NE(out.find("Internal error: no CLI actor"), std::string::npos);
}
