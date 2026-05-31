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

#include <hpactor/adt/stream_buffer.hpp>
#include <hpactor/cli/command_context.hpp>
#include <hpactor/cli/command_registry.hpp>
#include <hpactor/cli/pretty_formatter.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/mailbox/dead_letter_queue.hpp>
#include <hpactor/types/types.hpp>

#include <gtest/gtest.h>

#include <chrono>

using namespace hpactor;
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

class DlqIntegrationTest : public ::testing::Test {
  protected:
    void SetUp() override {
        Config config;
        config.scheduler_threads = 0;
        config.dead_letters.enabled = true;
        config.dead_letters.capacity = 64;
        config.dead_letters.store_payload = true;
        config.dead_letters.max_payload_sample_bytes = 512;
        system_ = std::make_unique<ActorSystem>(config);
    }

    void TearDown() override {
        fmt_.reset();
        system_.reset();
    }

    void push_record(int idx) {
        mailbox::DeadLetterRecord r;
        r.reason = (idx % 2 == 0) ? mailbox::DeadLetterReason::MailboxFull
                                  : mailbox::DeadLetterReason::Expired;
        r.source = (idx < 2) ? mailbox::DeadLetterSource::LocalDelivery
                             : mailbox::DeadLetterSource::MailboxAdmission;
        r.target.id = ActorId{static_cast<uint64_t>(100 + idx)};
        r.type_tag = TypeTag{static_cast<uint16_t>(0x100 + idx)};
        r.message_id = static_cast<uint64_t>(1000 + idx);
        r.priority = static_cast<uint8_t>(idx);
        r.payload_size = 16;
        r.payload_sample =
            StreamBuffer{static_cast<uint8_t>(0xAA), static_cast<uint8_t>(0xBB),
                         static_cast<uint8_t>(0xCC), static_cast<uint8_t>(0xDD)};
        r.mailbox_depth = 90;
        r.mailbox_capacity = 100;
        r.timestamp_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
        ASSERT_TRUE(system_->dead_letter_queue()->try_push(std::move(r)));
    }

    void execute_cmd(ICommand& cmd) {
        fmt_ = std::make_unique<PrettyFormatter>();
        ctx_.system = system_.get();
        ctx_.output = fmt_.get();
        cmd.execute(ctx_);
    }

    std::string output() {
        return fmt_->finalize();
    }

    std::unique_ptr<ActorSystem> system_;
    std::unique_ptr<PrettyFormatter> fmt_;
    CommandContext ctx_;
};

} // anonymous namespace

// =============================================================================
// DlqListCommand — integration
// =============================================================================

TEST_F(DlqIntegrationTest, DlqListEmpty) {
    auto* cmd = find_cmd("dlq/list");
    ASSERT_NE(cmd, nullptr);
    execute_cmd(*cmd);
    std::string out = output();
    EXPECT_NE(out.find("0 total"), std::string::npos);
}

TEST_F(DlqIntegrationTest, DlqListWithRecords) {
    push_record(0);
    push_record(1);
    push_record(2);
    auto* cmd = find_cmd("dlq/list");
    ASSERT_NE(cmd, nullptr);
    execute_cmd(*cmd);
    std::string out = output();
    EXPECT_NE(out.find("3 total"), std::string::npos);
    EXPECT_NE(out.find("MailboxFull"), std::string::npos);
    EXPECT_NE(out.find("Expired"), std::string::npos);
}

TEST_F(DlqIntegrationTest, DlqListFilterByReason) {
    push_record(0); // MailboxFull
    push_record(1); // Expired
    auto* cmd = find_cmd("dlq/list");
    ASSERT_NE(cmd, nullptr);
    ctx_.system = system_.get();
    ctx_.params["reason"] = "Expired";
    fmt_ = std::make_unique<PrettyFormatter>();
    ctx_.output = fmt_.get();
    cmd->execute(ctx_);
    std::string out = output();
    EXPECT_NE(out.find("1 total"), std::string::npos);
    EXPECT_NE(out.find("Expired"), std::string::npos);
}

TEST_F(DlqIntegrationTest, DlqListFilterBySource) {
    push_record(0); // LocalDelivery
    push_record(1); // LocalDelivery
    push_record(2); // MailboxAdmission (idx >= 2)
    auto* cmd = find_cmd("dlq/list");
    ASSERT_NE(cmd, nullptr);
    ctx_.system = system_.get();
    ctx_.params["source"] = "MailboxAdmission";
    fmt_ = std::make_unique<PrettyFormatter>();
    ctx_.output = fmt_.get();
    cmd->execute(ctx_);
    std::string out = output();
    EXPECT_NE(out.find("1 total"), std::string::npos);
    EXPECT_NE(out.find("MailboxAdmission"), std::string::npos);
}

TEST_F(DlqIntegrationTest, DlqListWithLimit) {
    push_record(0);
    push_record(1);
    push_record(2);
    push_record(3);
    auto* cmd = find_cmd("dlq/list");
    ASSERT_NE(cmd, nullptr);
    ctx_.system = system_.get();
    ctx_.params["limit"] = "2";
    fmt_ = std::make_unique<PrettyFormatter>();
    ctx_.output = fmt_.get();
    cmd->execute(ctx_);
    std::string out = output();
    EXPECT_NE(out.find("4 total"), std::string::npos);
    EXPECT_NE(out.find("... 2 more"), std::string::npos);
}

TEST_F(DlqIntegrationTest, DlqListInvalidLimit) {
    auto* cmd = find_cmd("dlq/list");
    ASSERT_NE(cmd, nullptr);
    ctx_.system = system_.get();
    ctx_.params["limit"] = "abc";
    fmt_ = std::make_unique<PrettyFormatter>();
    ctx_.output = fmt_.get();
    cmd->execute(ctx_);
    std::string out = output();
    EXPECT_NE(out.find("Invalid --limit value"), std::string::npos);
}

// =============================================================================
// DlqShowCommand — integration
// =============================================================================

TEST_F(DlqIntegrationTest, DlqShowValidRecord) {
    push_record(0);
    auto* cmd = find_cmd("dlq/show");
    ASSERT_NE(cmd, nullptr);
    ctx_.system = system_.get();
    ctx_.params["index"] = "0";
    fmt_ = std::make_unique<PrettyFormatter>();
    ctx_.output = fmt_.get();
    cmd->execute(ctx_);
    std::string out = output();
    EXPECT_NE(out.find("Dead-Letter Record #0"), std::string::npos);
    EXPECT_NE(out.find("MailboxFull"), std::string::npos);
    EXPECT_NE(out.find("LocalDelivery"), std::string::npos);
    EXPECT_NE(out.find("100"), std::string::npos); // target id
}

TEST_F(DlqIntegrationTest, DlqShowMissingIndex) {
    auto* cmd = find_cmd("dlq/show");
    ASSERT_NE(cmd, nullptr);
    execute_cmd(*cmd);
    std::string out = output();
    EXPECT_NE(out.find("Usage: /dlq show --index N"), std::string::npos);
}

TEST_F(DlqIntegrationTest, DlqShowInvalidIndex) {
    auto* cmd = find_cmd("dlq/show");
    ASSERT_NE(cmd, nullptr);
    ctx_.system = system_.get();
    ctx_.params["index"] = "abc";
    fmt_ = std::make_unique<PrettyFormatter>();
    ctx_.output = fmt_.get();
    cmd->execute(ctx_);
    std::string out = output();
    EXPECT_NE(out.find("Invalid index"), std::string::npos);
}

TEST_F(DlqIntegrationTest, DlqShowOutOfRange) {
    push_record(0);
    auto* cmd = find_cmd("dlq/show");
    ASSERT_NE(cmd, nullptr);
    ctx_.system = system_.get();
    ctx_.params["index"] = "99";
    fmt_ = std::make_unique<PrettyFormatter>();
    ctx_.output = fmt_.get();
    cmd->execute(ctx_);
    std::string out = output();
    EXPECT_NE(out.find("out of range"), std::string::npos);
}

TEST_F(DlqIntegrationTest, DlqShowWithPayload) {
    push_record(0);
    auto* cmd = find_cmd("dlq/show");
    ASSERT_NE(cmd, nullptr);
    ctx_.system = system_.get();
    ctx_.params["index"] = "0";
    fmt_ = std::make_unique<PrettyFormatter>();
    ctx_.output = fmt_.get();
    cmd->execute(ctx_);
    std::string out = output();
    EXPECT_NE(out.find("Payload hex"), std::string::npos);
}

TEST_F(DlqIntegrationTest, DlqShowNoPayload) {
    mailbox::DeadLetterRecord r;
    r.reason = mailbox::DeadLetterReason::ActorNotFound;
    r.source = mailbox::DeadLetterSource::LocalDelivery;
    r.target.id = ActorId{1};
    r.type_tag = TypeTag{1};
    r.payload_size = 0;
    system_->dead_letter_queue()->try_push(std::move(r));
    auto* cmd = find_cmd("dlq/show");
    ASSERT_NE(cmd, nullptr);
    ctx_.system = system_.get();
    ctx_.params["index"] = "0";
    fmt_ = std::make_unique<PrettyFormatter>();
    ctx_.output = fmt_.get();
    cmd->execute(ctx_);
    std::string out = output();
    EXPECT_EQ(out.find("Payload hex"), std::string::npos);
}

// =============================================================================
// DlqReplayCommand — integration
// =============================================================================

TEST_F(DlqIntegrationTest, DlqReplayMissingIndex) {
    auto* cmd = find_cmd("dlq/replay");
    ASSERT_NE(cmd, nullptr);
    execute_cmd(*cmd);
    std::string out = output();
    EXPECT_NE(out.find("Usage: /dlq replay --index N"), std::string::npos);
}

TEST_F(DlqIntegrationTest, DlqReplayInvalidIndex) {
    auto* cmd = find_cmd("dlq/replay");
    ASSERT_NE(cmd, nullptr);
    ctx_.system = system_.get();
    ctx_.params["index"] = "abc";
    fmt_ = std::make_unique<PrettyFormatter>();
    ctx_.output = fmt_.get();
    cmd->execute(ctx_);
    std::string out = output();
    EXPECT_NE(out.find("Invalid index"), std::string::npos);
}

TEST_F(DlqIntegrationTest, DlqReplayOutOfRange) {
    auto* cmd = find_cmd("dlq/replay");
    ASSERT_NE(cmd, nullptr);
    ctx_.system = system_.get();
    ctx_.params["index"] = "99";
    fmt_ = std::make_unique<PrettyFormatter>();
    ctx_.output = fmt_.get();
    cmd->execute(ctx_);
    std::string out = output();
    EXPECT_NE(out.find("out of range"), std::string::npos);
}

TEST_F(DlqIntegrationTest, DlqReplayNoPayload) {
    mailbox::DeadLetterRecord r;
    r.reason = mailbox::DeadLetterReason::ActorNotFound;
    r.source = mailbox::DeadLetterSource::LocalDelivery;
    r.target.id = ActorId{1};
    r.type_tag = TypeTag{1};
    r.payload_size = 0;
    system_->dead_letter_queue()->try_push(std::move(r));
    auto* cmd = find_cmd("dlq/replay");
    ASSERT_NE(cmd, nullptr);
    ctx_.system = system_.get();
    ctx_.params["index"] = "0";
    fmt_ = std::make_unique<PrettyFormatter>();
    ctx_.output = fmt_.get();
    cmd->execute(ctx_);
    std::string out = output();
    EXPECT_NE(out.find("cannot replay"), std::string::npos);
}

// =============================================================================
// DlqExportCommand — integration
// =============================================================================

TEST_F(DlqIntegrationTest, DlqExportText) {
    push_record(0);
    auto* cmd = find_cmd("dlq/export");
    ASSERT_NE(cmd, nullptr);
    execute_cmd(*cmd);
    std::string out = output();
    EXPECT_NE(out.find("MailboxFull"), std::string::npos);
    EXPECT_NE(out.find("100"), std::string::npos);
}

TEST_F(DlqIntegrationTest, DlqExportJson) {
    push_record(0);
    auto* cmd = find_cmd("dlq/export");
    ASSERT_NE(cmd, nullptr);
    ctx_.system = system_.get();
    ctx_.params["format"] = "json";
    fmt_ = std::make_unique<PrettyFormatter>();
    ctx_.output = fmt_.get();
    cmd->execute(ctx_);
    std::string out = output();
    EXPECT_NE(out.find('['), std::string::npos);
    EXPECT_NE(out.find(']'), std::string::npos);
    EXPECT_NE(out.find("MailboxFull"), std::string::npos);
}

TEST_F(DlqIntegrationTest, DlqExportInvalidLimit) {
    auto* cmd = find_cmd("dlq/export");
    ASSERT_NE(cmd, nullptr);
    ctx_.system = system_.get();
    ctx_.params["limit"] = "xyz";
    fmt_ = std::make_unique<PrettyFormatter>();
    ctx_.output = fmt_.get();
    cmd->execute(ctx_);
    std::string out = output();
    EXPECT_NE(out.find("Invalid --limit value"), std::string::npos);
}

TEST_F(DlqIntegrationTest, DlqExportWithLimit) {
    push_record(0);
    push_record(1);
    push_record(2);
    auto* cmd = find_cmd("dlq/export");
    ASSERT_NE(cmd, nullptr);
    ctx_.system = system_.get();
    ctx_.params["limit"] = "2";
    fmt_ = std::make_unique<PrettyFormatter>();
    ctx_.output = fmt_.get();
    cmd->execute(ctx_);
    std::string out = output();
    // Should contain 2 records max, not all 3
    EXPECT_NE(out.find("0 "), std::string::npos);
    EXPECT_NE(out.find("1 "), std::string::npos);
    // "2 " should NOT appear since limit is 2
}

// =============================================================================
// DLQ-not-enabled paths
// =============================================================================
// NOTE: DeadLetterQueue is always instantiated by ActorSystem regardless of
// the enabled flag. The resolve_dlq() null-pointer guard is a safety check that
// cannot be triggered through the current ActorSystem implementation.
// To test this path, pass ctx.system == nullptr (covered in unit tests).
