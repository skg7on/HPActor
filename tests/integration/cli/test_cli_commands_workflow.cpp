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

#include <hpactor/actor/system/actor_system.hpp>
#include <hpactor/adt/stream_buffer.hpp>
#include <hpactor/cli/command/cli_session.hpp>
#include <hpactor/cli/command/command_context.hpp>
#include <hpactor/cli/command/command_node.hpp>
#include <hpactor/cli/command/command_registry.hpp>
#include <hpactor/cli/format/output_formatter.hpp>
#include <hpactor/cli/format/pretty_formatter.hpp>
#include <hpactor/msg/dead_letter_record.hpp>
#include <hpactor/types/types.hpp>

#include <gtest/gtest.h>

#include <map>
#include <memory>
#include <string>

using namespace hpactor;
using namespace hpactor::cli;

// mount_command is defined in cli_actor.cpp and exported for testing.
namespace hpactor {
namespace cli {
extern void mount_command(CommandNode* root, const ICommand& cmd);
}
} // namespace hpactor

namespace {

// ── Helpers ────────────────────────────────────────────────────────────────

ICommand* find_cmd(std::string_view path) {
    auto& reg = CommandRegistry::instance();
    for (auto& c : reg.commands()) {
        if (c->path() == path)
            return c.get();
    }
    return nullptr;
}

std::unique_ptr<CommandNode> build_command_tree() {
    auto root = std::make_unique<CommandNode>("", "root");
    auto& reg = CommandRegistry::instance();
    for (auto& c : reg.commands()) {
        mount_command(root.get(), *c);
    }
    return root;
}

// ── Test Fixture ───────────────────────────────────────────────────────────

class CliCommandsWorkflowTest : public ::testing::Test {
  protected:
    void SetUp() override {
        Config config;
        config.scheduler_threads = 0;
        config.dead_letters.enabled = true;
        config.dead_letters.capacity = 64;
        config.dead_letters.store_payload = true;
        config.dead_letters.max_payload_sample_bytes = 512;
        system_ = std::make_unique<ActorSystem>(config);
        tree_ = build_command_tree();
    }

    void TearDown() override {
        tree_.reset();
        system_.reset();
    }

    /// Process a command line through CliSession and return captured output.
    std::string process_line(const std::string& line) {
        std::string output;
        auto output_fn = [&](const std::string& s) { output += s; };
        auto formatter = OutputFormatter::create("pretty");
        CliSession session(system_.get(), tree_.get(), std::move(formatter),
                           output_fn);
        session.process_line(line);
        return output;
    }

    /// Execute a command directly (retrieved from registry) with system.
    std::string execute_direct(ICommand& cmd) {
        auto fmt = std::make_unique<PrettyFormatter>();
        CommandContext ctx;
        ctx.output = fmt.get();
        ctx.system = system_.get();
        cmd.execute(ctx);
        return fmt->finalize();
    }

    /// Execute a command directly with extra params.
    std::string
    execute_direct_with(ICommand& cmd,
                        const std::map<std::string, std::string>& params) {
        auto fmt = std::make_unique<PrettyFormatter>();
        CommandContext ctx;
        ctx.output = fmt.get();
        ctx.system = system_.get();
        for (auto& [k, v] : params)
            ctx.params[k] = v;
        cmd.execute(ctx);
        return fmt->finalize();
    }

    /// Push a DLQ record for integration tests.
    void push_dlq_record(int idx) {
        mailbox::DeadLetterRecord r;
        r.reason = (idx % 2 == 0) ? mailbox::DeadLetterReason::MailboxFull
                                  : mailbox::DeadLetterReason::Expired;
        r.source = (idx < 2) ? mailbox::DeadLetterSource::LocalDelivery
                             : mailbox::DeadLetterSource::MailboxAdmission;
        r.target.id = ActorId{static_cast<uint64_t>(100 + idx)};
        r.type_tag = TypeTag{static_cast<uint16_t>(0x100 + idx)};
        r.message_id = 1000ULL + static_cast<uint64_t>(idx);
        r.priority = static_cast<uint8_t>(idx);
        r.payload_size = 16;
        r.payload_sample =
            StreamBuffer{static_cast<uint8_t>(0xAA), static_cast<uint8_t>(0xBB),
                         static_cast<uint8_t>(0xCC), static_cast<uint8_t>(0xDD)};
        r.mailbox_depth = 90;
        r.mailbox_capacity = 100;
        r.timestamp_ns = 0;
        ASSERT_TRUE(system_->dead_letter_queue()->try_push(std::move(r)));
    }

    std::unique_ptr<ActorSystem> system_;
    std::unique_ptr<CommandNode> tree_;
};

} // anonymous namespace

// =============================================================================
// Group 1: Actor Commands (4 tests)
// --- Exercise the CliSession command tree for actor-related commands.
//     Without a CLI actor, these hit the "Internal error: no CLI actor" path,
//     but they verify tree traversal, parameter capture, and dispatch.
// =============================================================================

TEST_F(CliCommandsWorkflowTest, ActorShowCommandTree) {
    // /actor 0x1234 show — walks tree through actor -> <id> -> show
    std::string out = process_line("/actor 0x1234 show");
    // No CLI actor is attached, so the command hits the error branch.
    EXPECT_NE(out.find("Internal error: no CLI actor"), std::string::npos);
}

TEST_F(CliCommandsWorkflowTest, ActorListCommandTree) {
    // /actor list — walks tree through actor -> list
    std::string out = process_line("/actor list");
    EXPECT_NE(out.find("Internal error: no CLI actor"), std::string::npos);
}

TEST_F(CliCommandsWorkflowTest, ActorKillCommandTree) {
    // /actor 0x5678 kill — walks tree through actor -> <id> -> kill
    std::string out = process_line("/actor 0x5678 kill");
    EXPECT_NE(out.find("Internal error: no CLI actor"), std::string::npos);
}

TEST_F(CliCommandsWorkflowTest, ActorBackpressureCommandTree) {
    // /actor 0xABCD backpressure — mailbox-backpressure info via tree
    // Walks through actor -> <id> -> backpressure; tests the mailbox
    // inspection command flow.
    std::string out = process_line("/actor 0xABCD backpressure");
    EXPECT_NE(out.find("Internal error: no CLI actor"), std::string::npos);
}

// =============================================================================
// Group 2: System + DLQ Commands (4 tests)
// --- System commands use ctx.system directly; DLQ commands need records.
//     Exercise full CliSession pipeline with an active ActorSystem.
// =============================================================================

TEST_F(CliCommandsWorkflowTest, SystemStatsCommandTree) {
    // /system stats — uses ctx.system to read actor count + scheduler info
    std::string out = process_line("/system stats");
    EXPECT_NE(out.find("System Statistics"), std::string::npos);
    EXPECT_NE(out.find("Total actors"), std::string::npos);
}

TEST_F(CliCommandsWorkflowTest, SystemMemoryCommandTree) {
    // /system memory — exercises MemoryRegionRegistry via CliSession
    std::string out = process_line("/system memory");
    EXPECT_NE(out.find("Memory Regions"), std::string::npos);
    EXPECT_NE(out.find("Region"), std::string::npos);
    EXPECT_NE(out.find("Actor"), std::string::npos);
    EXPECT_NE(out.find("Message"), std::string::npos);
}

TEST_F(CliCommandsWorkflowTest, DlqListCommandTree) {
    // Populate DLQ with records, then list via CliSession tree
    push_dlq_record(0);
    push_dlq_record(1);
    std::string out = process_line("/dlq list");
    EXPECT_NE(out.find("Dead-Letter Queue Records"), std::string::npos);
    EXPECT_NE(out.find("2 total"), std::string::npos);
}

TEST_F(CliCommandsWorkflowTest, DlqShowCommandTree) {
    // Push a record and show it via --index N through CliSession
    push_dlq_record(0);
    std::string out = process_line("/dlq show --index 0");
    EXPECT_NE(out.find("Dead-Letter Record #0"), std::string::npos);
    EXPECT_NE(out.find("MailboxFull"), std::string::npos);
}

// =============================================================================
// Group 3: Fault + Ask + Help Commands (3 tests)
// =============================================================================

TEST_F(CliCommandsWorkflowTest, FaultStatusCommandTree) {
    // /fault status — reads FaultController from ActorSystem via CliSession
    std::string out = process_line("/fault status");
    EXPECT_NE(out.find("Fault Injection Status"), std::string::npos);
    EXPECT_NE(out.find("Enabled"), std::string::npos);
}

TEST_F(CliCommandsWorkflowTest, AskPendingCommandTree) {
    // /ask pending — standalone command, no host required
    std::string out = process_line("/ask pending");
    EXPECT_NE(out.find("Pending Ask Requests"), std::string::npos);
    EXPECT_NE(out.find("not yet available"), std::string::npos);
}

TEST_F(CliCommandsWorkflowTest, HelpCommandTree) {
    // /help — renders the full command tree help text
    std::string out = process_line("/help");
    EXPECT_NE(out.find("Available Commands"), std::string::npos);
    // The help text should list known top-level command groups.
    EXPECT_NE(out.find("actor"), std::string::npos);
    EXPECT_NE(out.find("system"), std::string::npos);
}

// =============================================================================
// Group 4: Error Handling (2 tests)
// =============================================================================

TEST_F(CliCommandsWorkflowTest, UnknownCommandShowsError) {
    // /bogus is not in the command tree — CliSession must report it.
    std::string out = process_line("/bogus");
    EXPECT_NE(out.find("Unknown command"), std::string::npos);
}

TEST_F(CliCommandsWorkflowTest, InvalidArgumentsHandling) {
    // ── 4a: actor show with missing actor ID ────────────────────────────
    {
        auto* cmd = find_cmd("actor/<id>/show");
        ASSERT_NE(cmd, nullptr);
        std::string out = execute_direct(*cmd);
        EXPECT_NE(out.find("Missing actor ID"), std::string::npos);
    }

    // ── 4b: actor show with garbage actor ID ────────────────────────────
    {
        auto* cmd = find_cmd("actor/<id>/show");
        ASSERT_NE(cmd, nullptr);
        std::string out = execute_direct_with(*cmd, {{"<id>", "not_a_number"}});
        EXPECT_NE(out.find("Invalid actor ID"), std::string::npos);
    }

    // ── 4c: dlq show with missing --index flag ──────────────────────────
    {
        auto* cmd = find_cmd("dlq/show");
        ASSERT_NE(cmd, nullptr);
        std::string out = execute_direct(*cmd);
        EXPECT_NE(out.find("Usage: /dlq show --index"), std::string::npos);
    }

    // ── 4d: dlq show with invalid index ─────────────────────────────────
    {
        auto* cmd = find_cmd("dlq/show");
        ASSERT_NE(cmd, nullptr);
        std::string out = execute_direct_with(*cmd, {{"index", "not_a_number"}});
        EXPECT_NE(out.find("Invalid index"), std::string::npos);
    }

    // ── 4e: dlq show with out-of-range index ────────────────────────────
    {
        auto* cmd = find_cmd("dlq/show");
        ASSERT_NE(cmd, nullptr);
        std::string out = execute_direct_with(*cmd, {{"index", "999"}});
        EXPECT_NE(out.find("out of range"), std::string::npos);
    }

    // ── 4f: system stop with missing actor_id ───────────────────────────
    {
        auto* cmd = find_cmd("system/stop/<actor_id>");
        ASSERT_NE(cmd, nullptr);
        std::string out = execute_direct(*cmd);
        EXPECT_NE(out.find("Missing actor ID"), std::string::npos);
    }
}
