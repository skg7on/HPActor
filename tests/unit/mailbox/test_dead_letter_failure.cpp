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

#include <hpactor/mailbox/dead_letter_queue.hpp>
#include <hpactor/types/failure_envelope.hpp>

// failure_reason(DeadLetterReason) mapping
TEST(DeadLetterFailureTest, FailureReasonMapping) {
    using namespace hpactor::mailbox;

    EXPECT_EQ(failure_reason(DeadLetterReason::MailboxFull),
              hpactor::FailureReason::MailboxFull);
    EXPECT_EQ(failure_reason(DeadLetterReason::MailboxClosed),
              hpactor::FailureReason::MailboxClosed);
    EXPECT_EQ(failure_reason(DeadLetterReason::ActorNotFound),
              hpactor::FailureReason::NoRoute);
    EXPECT_EQ(failure_reason(DeadLetterReason::ActorTerminated),
              hpactor::FailureReason::ActorDead);
    EXPECT_EQ(failure_reason(DeadLetterReason::MissingRoute),
              hpactor::FailureReason::NoRoute);
    EXPECT_EQ(failure_reason(DeadLetterReason::RemoteNodeUnreachable),
              hpactor::FailureReason::NodeUnavailable);
    EXPECT_EQ(failure_reason(DeadLetterReason::NetworkPartition),
              hpactor::FailureReason::NodeUnavailable);
    EXPECT_EQ(failure_reason(DeadLetterReason::TransportSendFailed),
              hpactor::FailureReason::TransportError);
    EXPECT_EQ(failure_reason(DeadLetterReason::DecodeFailed),
              hpactor::FailureReason::SerializationError);
    EXPECT_EQ(failure_reason(DeadLetterReason::OverflowPolicy),
              hpactor::FailureReason::RejectedByPolicy);
    EXPECT_EQ(failure_reason(DeadLetterReason::NoDropRejected),
              hpactor::FailureReason::RejectedByPolicy);
    EXPECT_EQ(failure_reason(DeadLetterReason::DrainTimeout),
              hpactor::FailureReason::Timeout);
    EXPECT_EQ(failure_reason(DeadLetterReason::DrainPolicyDrop),
              hpactor::FailureReason::Dropped);
}

// failure_source(DeadLetterSource) mapping
TEST(DeadLetterFailureTest, FailureSourceMapping) {
    using namespace hpactor::mailbox;

    EXPECT_EQ(failure_source(DeadLetterSource::LocalDelivery),
              hpactor::FailureSource::ActorRuntime);
    EXPECT_EQ(failure_source(DeadLetterSource::RemoteDelivery),
              hpactor::FailureSource::Transport);
    EXPECT_EQ(failure_source(DeadLetterSource::MailboxAdmission),
              hpactor::FailureSource::Mailbox);
    EXPECT_EQ(failure_source(DeadLetterSource::ServiceDiscovery),
              hpactor::FailureSource::Discovery);
}

// DeadLetterRecord::to_failure_envelope()
TEST(DeadLetterFailureTest, ToFailureEnvelope) {
    using namespace hpactor::mailbox;

    DeadLetterRecord dl;
    dl.reason = DeadLetterReason::MailboxFull;
    dl.source = DeadLetterSource::MailboxAdmission;
    dl.message_id = 42;
    dl.timestamp_ns = 12345;

    auto env = dl.to_failure_envelope();
    EXPECT_EQ(env.reason, hpactor::FailureReason::MailboxFull);
    EXPECT_EQ(env.source, hpactor::FailureSource::Mailbox);
    EXPECT_EQ(env.message_id, hpactor::MessageId{42});
    EXPECT_EQ(env.timestamp_ns, 12345);
    EXPECT_EQ(env.retryable, true); // MailboxFull is retryable
}
