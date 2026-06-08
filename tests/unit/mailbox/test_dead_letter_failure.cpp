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

#include <hpactor/msg/dead_letter_record.hpp>
#include <hpactor/msg/failure_envelope.hpp>

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

TEST(DeadLetterFailureTest, ToStringDeadLetterReason) {
    using namespace hpactor::mailbox;
    EXPECT_STREQ(to_string(DeadLetterReason::MailboxFull), "MailboxFull");
    EXPECT_STREQ(to_string(DeadLetterReason::OverflowPolicy), "OverflowPolicy");
    EXPECT_STREQ(to_string(DeadLetterReason::Expired), "Expired");
    EXPECT_STREQ(to_string(DeadLetterReason::DrainTimeout), "DrainTimeout");
}

TEST(DeadLetterFailureTest, ToStringDeadLetterSource) {
    using namespace hpactor::mailbox;
    EXPECT_STREQ(to_string(DeadLetterSource::LocalDelivery), "LocalDelivery");
    EXPECT_STREQ(to_string(DeadLetterSource::MailboxAdmission), "MailboxAdmissi"
                                                                "on");
    EXPECT_STREQ(to_string(DeadLetterSource::Replay), "Replay");
}

// ── DeadLetterRoutingPolicy ────────────────────────────────────────────

TEST(DeadLetterRoutingPolicyTest, DefaultConstructedIsNever) {
    using namespace hpactor::mailbox;
    DeadLetterRoutingPolicy policy{};
    // Zero-initialised enum naturally maps to Never=0. The safe production
    // default (Always) is enforced by DeadLetterConfig, not the raw enum.
    EXPECT_EQ(policy, DeadLetterRoutingPolicy::Never);
}

TEST(DeadLetterRoutingPolicyTest, Uint8Size) {
    EXPECT_EQ(sizeof(hpactor::mailbox::DeadLetterRoutingPolicy), 1);
}

// ── should_route_to_dlq decision matrix ────────────────────────────────

TEST(DeadLetterRoutingPolicyTest, NeverSuppressesAll) {
    using namespace hpactor::mailbox;
    EXPECT_FALSE(should_route_to_dlq(DeliveryMode::BestEffort,
                                     DeadLetterRoutingPolicy::Never));
    EXPECT_FALSE(should_route_to_dlq(DeliveryMode::ObservableBestEffort,
                                     DeadLetterRoutingPolicy::Never));
    EXPECT_FALSE(should_route_to_dlq(DeliveryMode::AtLeastOnce,
                                     DeadLetterRoutingPolicy::Never));
    EXPECT_FALSE(should_route_to_dlq(DeliveryMode::DurableAtLeastOnce,
                                     DeadLetterRoutingPolicy::Never));
}

TEST(DeadLetterRoutingPolicyTest, AlwaysRoutesAll) {
    using namespace hpactor::mailbox;
    EXPECT_TRUE(should_route_to_dlq(DeliveryMode::BestEffort,
                                    DeadLetterRoutingPolicy::Always));
    EXPECT_TRUE(should_route_to_dlq(DeliveryMode::ObservableBestEffort,
                                    DeadLetterRoutingPolicy::Always));
    EXPECT_TRUE(should_route_to_dlq(DeliveryMode::AtLeastOnce,
                                    DeadLetterRoutingPolicy::Always));
    EXPECT_TRUE(should_route_to_dlq(DeliveryMode::DurableAtLeastOnce,
                                    DeadLetterRoutingPolicy::Always));
}

TEST(DeadLetterRoutingPolicyTest, TrackedOnlyRoutesAtLeastOnceAndAbove) {
    using namespace hpactor::mailbox;
    EXPECT_FALSE(should_route_to_dlq(DeliveryMode::BestEffort,
                                     DeadLetterRoutingPolicy::TrackedOnly));
    EXPECT_FALSE(should_route_to_dlq(DeliveryMode::ObservableBestEffort,
                                     DeadLetterRoutingPolicy::TrackedOnly));
    EXPECT_TRUE(should_route_to_dlq(DeliveryMode::AtLeastOnce,
                                    DeadLetterRoutingPolicy::TrackedOnly));
    EXPECT_TRUE(should_route_to_dlq(DeliveryMode::DurableAtLeastOnce,
                                    DeadLetterRoutingPolicy::TrackedOnly));
}

TEST(DeadLetterRoutingPolicyTest, ObservableAndAboveRoutesObservableAndAbove) {
    using namespace hpactor::mailbox;
    EXPECT_FALSE(should_route_to_dlq(
        DeliveryMode::BestEffort, DeadLetterRoutingPolicy::ObservableAndAbove));
    EXPECT_TRUE(should_route_to_dlq(DeliveryMode::ObservableBestEffort,
                                    DeadLetterRoutingPolicy::ObservableAndAbove));
    EXPECT_TRUE(should_route_to_dlq(DeliveryMode::AtLeastOnce,
                                    DeadLetterRoutingPolicy::ObservableAndAbove));
    EXPECT_TRUE(should_route_to_dlq(DeliveryMode::DurableAtLeastOnce,
                                    DeadLetterRoutingPolicy::ObservableAndAbove));
}

TEST(DeadLetterRoutingPolicyTest, ToStringRoundTrip) {
    using namespace hpactor::mailbox;
    EXPECT_STREQ(to_string(DeadLetterRoutingPolicy::Never), "never");
    EXPECT_STREQ(to_string(DeadLetterRoutingPolicy::TrackedOnly), "tracked_"
                                                                  "only");
    EXPECT_STREQ(to_string(DeadLetterRoutingPolicy::ObservableAndAbove), "obser"
                                                                         "vable"
                                                                         "_and_"
                                                                         "abov"
                                                                         "e");
    EXPECT_STREQ(to_string(DeadLetterRoutingPolicy::Always), "always");
}

TEST(DeadLetterRoutingPolicyTest, ParseFromString) {
    using namespace hpactor::mailbox;
    EXPECT_EQ(parse_routing_policy("never"), DeadLetterRoutingPolicy::Never);
    EXPECT_EQ(parse_routing_policy("tracked_only"),
              DeadLetterRoutingPolicy::TrackedOnly);
    EXPECT_EQ(parse_routing_policy("observable_and_above"),
              DeadLetterRoutingPolicy::ObservableAndAbove);
    EXPECT_EQ(parse_routing_policy("always"), DeadLetterRoutingPolicy::Always);
    EXPECT_EQ(parse_routing_policy("garbage"), DeadLetterRoutingPolicy::Always);
    EXPECT_EQ(parse_routing_policy(""), DeadLetterRoutingPolicy::Always);
}

// ── DeadLetterConfig routing_policy field ─────────────────────────────

TEST(DeadLetterConfigRoutingPolicyTest, DefaultConfigRoutesAlways) {
    hpactor::mailbox::DeadLetterConfig cfg{};
    EXPECT_TRUE(cfg.enabled);
    EXPECT_EQ(cfg.routing_policy, hpactor::mailbox::DeadLetterRoutingPolicy::Always);
}

TEST(DeadLetterConfigRoutingPolicyTest, ExplicitRoutingPolicyRoundTrip) {
    hpactor::mailbox::DeadLetterConfig cfg{};
    cfg.routing_policy = hpactor::mailbox::DeadLetterRoutingPolicy::Never;
    EXPECT_EQ(cfg.routing_policy, hpactor::mailbox::DeadLetterRoutingPolicy::Never);
    cfg.routing_policy = hpactor::mailbox::DeadLetterRoutingPolicy::TrackedOnly;
    EXPECT_EQ(cfg.routing_policy,
              hpactor::mailbox::DeadLetterRoutingPolicy::TrackedOnly);
    cfg.routing_policy =
        hpactor::mailbox::DeadLetterRoutingPolicy::ObservableAndAbove;
    EXPECT_EQ(cfg.routing_policy,
              hpactor::mailbox::DeadLetterRoutingPolicy::ObservableAndAbove);
}
