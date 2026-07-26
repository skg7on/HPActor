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

#include <hpactor/mailbox/mailbox_policy.hpp>
#include <hpactor/net/reliable_ack.hpp>
#include <hpactor/net/reliable_ack_emission.hpp>

namespace {

using namespace hpactor;
using namespace hpactor::net;
using namespace hpactor::mailbox;

// ── ACK decision logic ──────────────────────────────────────────────────

TEST(ReliableAckEmissionTest, NoEmitWhenAckNotRequested) {
    // When AckRequested flag is NOT set, no ACK/NACK should be emitted.
    EnqueueResult accepted;
    accepted.code = EnqueueResultCode::Accepted;

    auto decision = compute_ack_emission(accepted, false);
    EXPECT_FALSE(decision.should_emit);
}

TEST(ReliableAckEmissionTest, EmitAckOnAcceptedDelivery) {
    // When message is accepted and AckRequested is set, emit ACK Accepted.
    EnqueueResult accepted;
    accepted.code = EnqueueResultCode::Accepted;

    auto decision = compute_ack_emission(accepted, true);
    EXPECT_TRUE(decision.should_emit);
    EXPECT_EQ(decision.status, AckStatus::Accepted);
    EXPECT_EQ(decision.retry_after_ms, 0);
}

TEST(ReliableAckEmissionTest, EmitNackMailboxFullOnRejected) {
    // Rejected at capacity → NACK (retryable with backoff hint).
    EnqueueResult rejected;
    rejected.code = EnqueueResultCode::Rejected;
    rejected.retry_after = std::chrono::milliseconds(250);

    auto decision = compute_ack_emission(rejected, true);
    EXPECT_TRUE(decision.should_emit);
    EXPECT_EQ(decision.status, AckStatus::Rejected);
    EXPECT_EQ(decision.retry_after_ms, 250);
}

TEST(ReliableAckEmissionTest, EmitNackOnActorNotFound) {
    // Actor not found → NACK (non-retryable).
    EnqueueResult not_found;
    not_found.code = EnqueueResultCode::ActorNotFound;

    auto decision = compute_ack_emission(not_found, true);
    EXPECT_TRUE(decision.should_emit);
    EXPECT_EQ(decision.status, AckStatus::Rejected);
    EXPECT_EQ(decision.retry_after_ms, 0);
}

TEST(ReliableAckEmissionTest, EmitNackOnMailboxClosed) {
    // Mailbox closed → NACK (non-retryable).
    EnqueueResult closed;
    closed.code = EnqueueResultCode::MailboxClosed;

    auto decision = compute_ack_emission(closed, true);
    EXPECT_TRUE(decision.should_emit);
    EXPECT_EQ(decision.status, AckStatus::Rejected);
    EXPECT_EQ(decision.retry_after_ms, 0);
}

TEST(ReliableAckEmissionTest, EmitNackOnOtherRejection) {
    // Generic rejection (e.g., DroppedNewest) → NACK (non-retryable).
    EnqueueResult dropped;
    dropped.code = EnqueueResultCode::DroppedNewest;

    auto decision = compute_ack_emission(dropped, true);
    EXPECT_TRUE(decision.should_emit);
    EXPECT_EQ(decision.status, AckStatus::Rejected);
    EXPECT_EQ(decision.retry_after_ms, 0);
}

TEST(ReliableAckEmissionTest, EmitNackOnReroutedToDeadLetter) {
    // Rerouted to DLQ → NACK (non-retryable).
    EnqueueResult dlq_routed;
    dlq_routed.code = EnqueueResultCode::ReroutedToDeadLetter;

    auto decision = compute_ack_emission(dlq_routed, true);
    EXPECT_TRUE(decision.should_emit);
    EXPECT_EQ(decision.status, AckStatus::Rejected);
    EXPECT_EQ(decision.retry_after_ms, 0);
}

TEST(ReliableAckEmissionTest, RetryAfterZeroWhenNoBackoffHint) {
    // When retry_after is zero (no backpressure hint), retry_after_ms = 0.
    EnqueueResult rejected;
    rejected.code = EnqueueResultCode::Rejected;
    rejected.retry_after = std::chrono::milliseconds(0);

    auto decision = compute_ack_emission(rejected, true);
    EXPECT_TRUE(decision.should_emit);
    EXPECT_EQ(decision.retry_after_ms, 0);
}

} // namespace
