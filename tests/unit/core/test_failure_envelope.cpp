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

#include <gtest/gtest.h>

#include <hpactor/msg/failure_envelope.hpp>

#include <string>

// ── Default-constructed envelope ────────────────────────────────────
TEST(FailureEnvelopeTest, DefaultConstruction) {
    hpactor::FailureEnvelope env;
    EXPECT_EQ(env.reason, hpactor::FailureReason::Unknown);
    EXPECT_EQ(env.actor_id, hpactor::ActorId{});
    EXPECT_EQ(env.message_id, hpactor::MessageId{});
    EXPECT_EQ(env.retryable, false);
    EXPECT_EQ(env.timestamp_ns, 0);
    EXPECT_EQ(env.detail_len, 0);
    EXPECT_EQ(env.source, hpactor::FailureSource::ActorRuntime);
}

// ── make_failure_envelope() fills all fields ────────────────────────
TEST(FailureEnvelopeTest, MakeFailureEnvelopeFillsAllFields) {
    hpactor::ActorId target_id{42};
    hpactor::ActorAddress sender_addr;
    hpactor::ActorAddress receiver_addr;
    hpactor::MessageId msg_id{100};
    hpactor::TraceContext trace_ctx;

    auto env = hpactor::make_failure_envelope(
        hpactor::FailureReason::MailboxFull, target_id, sender_addr,
        receiver_addr, msg_id, trace_ctx, hpactor::FailureSource::Mailbox,
        "depth=1024 capacity=1024");

    EXPECT_EQ(env.reason, hpactor::FailureReason::MailboxFull);
    EXPECT_EQ(env.actor_id, target_id);
    EXPECT_EQ(env.message_id, msg_id);
    EXPECT_EQ(env.retryable, true); // MailboxFull is retryable
    EXPECT_GT(env.timestamp_ns, 0);
    EXPECT_EQ(env.source, hpactor::FailureSource::Mailbox);
    EXPECT_GT(env.detail_len, 0);
    EXPECT_EQ(env.detail_view(), "depth=1024 capacity=1024");
}

// ── Non-retryable reason ────────────────────────────────────────────
TEST(FailureEnvelopeTest, NonRetryableReason) {
    hpactor::ActorId target{1};
    hpactor::ActorAddress addr;
    hpactor::MessageId mid;
    hpactor::TraceContext tc;

    auto env = hpactor::make_failure_envelope(
        hpactor::FailureReason::ActorDead, target, addr, addr, mid, tc,
        hpactor::FailureSource::ActorRuntime, "");

    EXPECT_EQ(env.reason, hpactor::FailureReason::ActorDead);
    EXPECT_EQ(env.retryable, false);
    EXPECT_EQ(env.detail_len, 0);
}

// ── set_detail() truncation at 255 chars ────────────────────────────
TEST(FailureEnvelopeTest, SetDetailTruncationAt255) {
    hpactor::FailureEnvelope env;
    std::string str255(255, 'y');
    env.set_detail(str255);
    EXPECT_EQ(env.detail_len, 255);
    // detail is null-terminated after set_detail
    EXPECT_EQ(env.detail[255], '\0');
}

// ── set_detail() truncation when string exceeds array ───────────────
TEST(FailureEnvelopeTest, SetDetailTruncationExceedsArray) {
    hpactor::FailureEnvelope env;
    std::string long_str(300, 'x');
    env.set_detail(long_str);
    EXPECT_EQ(env.detail_len, 255); // capped at detail.size() - 1
    EXPECT_EQ(env.detail[255], '\0');
}

// ── Overwrite set_detail preserves null terminator ──────────────────
TEST(FailureEnvelopeTest, OverwriteSetDetailPreservesNullTerminator) {
    hpactor::FailureEnvelope env;
    env.set_detail("hello");
    EXPECT_EQ(env.detail_len, 5);
    EXPECT_EQ(env.detail_view(), "hello");
    env.set_detail("hi");
    EXPECT_EQ(env.detail_len, 2);
    EXPECT_EQ(env.detail_view(), "hi");
    EXPECT_EQ(env.detail[2], '\0'); // null terminator at new position
}

// ── retryable flag matches retryable(FailureReason) ─────────────────
TEST(FailureEnvelopeTest, RetryableFlagMatchesFunction) {
    hpactor::ActorId id{1};
    hpactor::ActorAddress a;
    hpactor::MessageId m;
    hpactor::TraceContext t;

    auto env = hpactor::make_failure_envelope(
        hpactor::FailureReason::Timeout, id, a, a, m, t,
        hpactor::FailureSource::ActorRuntime, "");

    EXPECT_EQ(env.retryable, hpactor::retryable(env.reason));
}

// ── Factory with no detail ──────────────────────────────────────────
TEST(FailureEnvelopeTest, FactoryWithNoDetail) {
    hpactor::ActorId id{7};
    hpactor::ActorAddress a;
    hpactor::MessageId m{42};
    hpactor::TraceContext t;

    auto env =
        hpactor::make_failure_envelope(hpactor::FailureReason::NoRoute, id, a, a,
                                       m, t, hpactor::FailureSource::ActorRuntime);

    EXPECT_EQ(env.reason, hpactor::FailureReason::NoRoute);
    EXPECT_EQ(env.detail_len, 0);
    EXPECT_TRUE(env.detail_view().empty());
}
