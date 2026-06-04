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

#include <hpactor/mailbox/delivery_result.hpp>

#include <gtest/gtest.h>

namespace hpactor::mailbox {
namespace {

// ── DeliveryStatus enum values ───────────────────────────────────────────

TEST(DeliveryStatusTest, ExplicitValues) {
    EXPECT_EQ(static_cast<uint8_t>(DeliveryStatus::Accepted), 0);
    EXPECT_EQ(static_cast<uint8_t>(DeliveryStatus::AcceptedWithPressure), 1);
    EXPECT_EQ(static_cast<uint8_t>(DeliveryStatus::NoRoute), 2);
    EXPECT_EQ(static_cast<uint8_t>(DeliveryStatus::ActorDead), 3);
    EXPECT_EQ(static_cast<uint8_t>(DeliveryStatus::MailboxFull), 4);
    EXPECT_EQ(static_cast<uint8_t>(DeliveryStatus::Expired), 5);
    EXPECT_EQ(static_cast<uint8_t>(DeliveryStatus::Duplicate), 6);
    EXPECT_EQ(static_cast<uint8_t>(DeliveryStatus::RemoteUnavailable), 7);
    EXPECT_EQ(static_cast<uint8_t>(DeliveryStatus::RejectedByPolicy), 8);
    EXPECT_EQ(static_cast<uint8_t>(DeliveryStatus::SerializationError), 9);
    EXPECT_EQ(static_cast<uint8_t>(DeliveryStatus::TransportError), 10);
    EXPECT_EQ(static_cast<uint8_t>(DeliveryStatus::ShuttingDown), 11);
}

TEST(DeliveryStatusTest, ToStringAllValues) {
    EXPECT_STREQ(to_string(DeliveryStatus::Accepted), "accepted");
    EXPECT_STREQ(to_string(DeliveryStatus::AcceptedWithPressure), "accepted_"
                                                                  "with_"
                                                                  "pressure");
    EXPECT_STREQ(to_string(DeliveryStatus::NoRoute), "no_route");
    EXPECT_STREQ(to_string(DeliveryStatus::ActorDead), "actor_dead");
    EXPECT_STREQ(to_string(DeliveryStatus::MailboxFull), "mailbox_full");
    EXPECT_STREQ(to_string(DeliveryStatus::Expired), "expired");
    EXPECT_STREQ(to_string(DeliveryStatus::Duplicate), "duplicate");
    EXPECT_STREQ(to_string(DeliveryStatus::RemoteUnavailable), "remote_"
                                                               "unavailable");
    EXPECT_STREQ(to_string(DeliveryStatus::RejectedByPolicy), "rejected_by_"
                                                              "policy");
    EXPECT_STREQ(to_string(DeliveryStatus::SerializationError), "serialization_"
                                                                "error");
    EXPECT_STREQ(to_string(DeliveryStatus::TransportError), "transport_error");
    EXPECT_STREQ(to_string(DeliveryStatus::ShuttingDown), "shutting_down");
}

TEST(DeliveryStatusTest, IsAccepted) {
    EXPECT_TRUE(is_accepted(DeliveryStatus::Accepted));
    EXPECT_TRUE(is_accepted(DeliveryStatus::AcceptedWithPressure));
    EXPECT_FALSE(is_accepted(DeliveryStatus::NoRoute));
    EXPECT_FALSE(is_accepted(DeliveryStatus::ActorDead));
    EXPECT_FALSE(is_accepted(DeliveryStatus::MailboxFull));
    EXPECT_FALSE(is_accepted(DeliveryStatus::Expired));
    EXPECT_FALSE(is_accepted(DeliveryStatus::Duplicate));
    EXPECT_FALSE(is_accepted(DeliveryStatus::RemoteUnavailable));
    EXPECT_FALSE(is_accepted(DeliveryStatus::RejectedByPolicy));
    EXPECT_FALSE(is_accepted(DeliveryStatus::SerializationError));
    EXPECT_FALSE(is_accepted(DeliveryStatus::TransportError));
    EXPECT_FALSE(is_accepted(DeliveryStatus::ShuttingDown));
}

TEST(DeliveryStatusTest, IsRetryable) {
    EXPECT_TRUE(is_retryable(DeliveryStatus::NoRoute));
    EXPECT_TRUE(is_retryable(DeliveryStatus::ActorDead));
    EXPECT_TRUE(is_retryable(DeliveryStatus::MailboxFull));
    EXPECT_TRUE(is_retryable(DeliveryStatus::RemoteUnavailable));
    EXPECT_TRUE(is_retryable(DeliveryStatus::TransportError));

    EXPECT_FALSE(is_retryable(DeliveryStatus::Accepted));
    EXPECT_FALSE(is_retryable(DeliveryStatus::AcceptedWithPressure));
    EXPECT_FALSE(is_retryable(DeliveryStatus::Expired));
    EXPECT_FALSE(is_retryable(DeliveryStatus::Duplicate));
    EXPECT_FALSE(is_retryable(DeliveryStatus::RejectedByPolicy));
    EXPECT_FALSE(is_retryable(DeliveryStatus::SerializationError));
    EXPECT_FALSE(is_retryable(DeliveryStatus::ShuttingDown));
}

TEST(DeliveryStatusTest, ToFailureReason) {
    EXPECT_EQ(to_failure_reason(DeliveryStatus::Accepted), FailureReason::Unknown);
    EXPECT_EQ(to_failure_reason(DeliveryStatus::AcceptedWithPressure),
              FailureReason::Unknown);
    EXPECT_EQ(to_failure_reason(DeliveryStatus::NoRoute), FailureReason::NoRoute);
    EXPECT_EQ(to_failure_reason(DeliveryStatus::ActorDead),
              FailureReason::ActorDead);
    EXPECT_EQ(to_failure_reason(DeliveryStatus::MailboxFull),
              FailureReason::MailboxFull);
    EXPECT_EQ(to_failure_reason(DeliveryStatus::Expired), FailureReason::Expired);
    EXPECT_EQ(to_failure_reason(DeliveryStatus::Duplicate),
              FailureReason::Duplicate);
    EXPECT_EQ(to_failure_reason(DeliveryStatus::RemoteUnavailable),
              FailureReason::RemoteUnavailable);
    EXPECT_EQ(to_failure_reason(DeliveryStatus::RejectedByPolicy),
              FailureReason::RejectedByPolicy);
    EXPECT_EQ(to_failure_reason(DeliveryStatus::SerializationError),
              FailureReason::SerializationError);
    EXPECT_EQ(to_failure_reason(DeliveryStatus::TransportError),
              FailureReason::TransportError);
    EXPECT_EQ(to_failure_reason(DeliveryStatus::ShuttingDown),
              FailureReason::ShuttingDown);
}

// ── DeliveryResult default construction and accessors ────────────────────

TEST(DeliveryResultTest, DefaultConstruction) {
    DeliveryResult dr;
    EXPECT_EQ(dr.status, DeliveryStatus::Accepted);
    EXPECT_EQ(dr.target, ActorAddress{});
    EXPECT_EQ(dr.message_id, MessageId{});
    EXPECT_EQ(dr.detail_code, 0u);
    EXPECT_TRUE(dr.ok());
    EXPECT_TRUE(dr.accepted());
    EXPECT_FALSE(dr.retryable());
    EXPECT_EQ(dr.failure_reason(), FailureReason::Unknown);
}

TEST(DeliveryResultTest, RejectedStatusAccessors) {
    DeliveryResult dr{DeliveryStatus::NoRoute, ActorAddress{}, MessageId{42}, 0};
    EXPECT_FALSE(dr.ok());
    EXPECT_FALSE(dr.accepted());
    EXPECT_TRUE(dr.retryable());
    EXPECT_EQ(dr.message_id, MessageId{42});
    EXPECT_EQ(dr.failure_reason(), FailureReason::NoRoute);
}

// ── DeliveryResult::from_enqueue() ───────────────────────────────────────

TEST(DeliveryResultTest, FromEnqueueAccepted) {
    EnqueueResult er;
    er.code = EnqueueResultCode::Accepted;
    ActorAddress addr;
    auto dr = DeliveryResult::from_enqueue(er, addr, MessageId{1});
    EXPECT_EQ(dr.status, DeliveryStatus::Accepted);
    EXPECT_TRUE(dr.ok());
    EXPECT_EQ(dr.message_id, MessageId{1});
}

TEST(DeliveryResultTest, FromEnqueueAcceptedWithSoftPressure) {
    EnqueueResult er;
    er.code = EnqueueResultCode::AcceptedWithSoftPressure;
    auto dr = DeliveryResult::from_enqueue(er, ActorAddress{}, {});
    EXPECT_EQ(dr.status, DeliveryStatus::AcceptedWithPressure);
    EXPECT_TRUE(dr.ok());
}

TEST(DeliveryResultTest, FromEnqueueRejected) {
    EnqueueResult er;
    er.code = EnqueueResultCode::Rejected;
    auto dr = DeliveryResult::from_enqueue(er, ActorAddress{}, {});
    EXPECT_EQ(dr.status, DeliveryStatus::MailboxFull);
    EXPECT_FALSE(dr.ok());
    EXPECT_TRUE(dr.retryable());
}

TEST(DeliveryResultTest, FromEnqueueDroppedNewest) {
    EnqueueResult er;
    er.code = EnqueueResultCode::DroppedNewest;
    auto dr = DeliveryResult::from_enqueue(er, ActorAddress{}, {});
    EXPECT_EQ(dr.status, DeliveryStatus::MailboxFull);
}

TEST(DeliveryResultTest, FromEnqueueDroppedExisting) {
    EnqueueResult er;
    er.code = EnqueueResultCode::DroppedExisting;
    auto dr = DeliveryResult::from_enqueue(er, ActorAddress{}, {});
    EXPECT_EQ(dr.status, DeliveryStatus::MailboxFull);
}

TEST(DeliveryResultTest, FromEnqueueReroutedToDeadLetter) {
    EnqueueResult er;
    er.code = EnqueueResultCode::ReroutedToDeadLetter;
    auto dr = DeliveryResult::from_enqueue(er, ActorAddress{}, {});
    EXPECT_EQ(dr.status, DeliveryStatus::RejectedByPolicy);
    EXPECT_FALSE(dr.retryable());
}

TEST(DeliveryResultTest, FromEnqueueReroutedToOverflow) {
    EnqueueResult er;
    er.code = EnqueueResultCode::ReroutedToOverflow;
    auto dr = DeliveryResult::from_enqueue(er, ActorAddress{}, {});
    EXPECT_EQ(dr.status, DeliveryStatus::AcceptedWithPressure);
    EXPECT_TRUE(dr.ok());
}

TEST(DeliveryResultTest, FromEnqueueMailboxClosed) {
    EnqueueResult er;
    er.code = EnqueueResultCode::MailboxClosed;
    auto dr = DeliveryResult::from_enqueue(er, ActorAddress{}, {});
    EXPECT_EQ(dr.status, DeliveryStatus::ActorDead);
    EXPECT_TRUE(dr.retryable());
}

TEST(DeliveryResultTest, FromEnqueueActorNotFound) {
    EnqueueResult er;
    er.code = EnqueueResultCode::ActorNotFound;
    auto dr = DeliveryResult::from_enqueue(er, ActorAddress{}, {});
    EXPECT_EQ(dr.status, DeliveryStatus::NoRoute);
    EXPECT_TRUE(dr.retryable());
}

TEST(DeliveryResultTest, FromEnqueueEndpointBackpressure) {
    EnqueueResult er;
    er.code = EnqueueResultCode::EndpointBackpressure;
    auto dr = DeliveryResult::from_enqueue(er, ActorAddress{}, {});
    EXPECT_EQ(dr.status, DeliveryStatus::RemoteUnavailable);
    EXPECT_TRUE(dr.retryable());
}

TEST(DeliveryResultTest, FromEnqueueEndpointCircuitOpen) {
    EnqueueResult er;
    er.code = EnqueueResultCode::EndpointCircuitOpen;
    auto dr = DeliveryResult::from_enqueue(er, ActorAddress{}, {});
    EXPECT_EQ(dr.status, DeliveryStatus::RemoteUnavailable);
    EXPECT_TRUE(dr.retryable()); // circuit breakers recover after cooldown
}

TEST(DeliveryResultTest, FromEnqueuePreservesTarget) {
    EnqueueResult er;
    er.code = EnqueueResultCode::Accepted;
    Ipv4Endpoint ep{0x7F000001, 8080};
    ActorAddress addr{EndPoint{ep}, ActorType{0}, ActorId{123}, 0};
    auto dr = DeliveryResult::from_enqueue(er, addr, MessageId{99});
    EXPECT_EQ(dr.target, addr);
    EXPECT_EQ(dr.message_id, MessageId{99});
}

// ── EnqueueResult::to_delivery_result() ──────────────────────────────────

TEST(EnqueueResultToDeliveryResultTest, ChainsToFromEnqueue) {
    EnqueueResult er;
    er.code = EnqueueResultCode::Accepted;
    Ipv4Endpoint ep{0x7F000001, 9090};
    ActorAddress addr{EndPoint{ep}, ActorType{0}, ActorId{1}, 0};
    auto dr = er.to_delivery_result(addr, MessageId{7});
    EXPECT_EQ(dr.status, DeliveryStatus::Accepted);
    EXPECT_EQ(dr.target, addr);
    EXPECT_EQ(dr.message_id, MessageId{7});
    EXPECT_TRUE(dr.ok());
}

// ── DeliveryResult::from_transport() ─────────────────────────────────────

TEST(DeliveryResultTest, FromTransportSent) {
    auto dr = DeliveryResult::from_transport(TransportSendResult::Sent,
                                             ActorAddress{}, MessageId{1});
    EXPECT_EQ(dr.status, DeliveryStatus::Accepted);
    EXPECT_TRUE(dr.ok());
}

TEST(DeliveryResultTest, FromTransportNotConnected) {
    auto dr = DeliveryResult::from_transport(TransportSendResult::NotConnected,
                                             ActorAddress{}, {});
    EXPECT_EQ(dr.status, DeliveryStatus::RemoteUnavailable);
    EXPECT_TRUE(dr.retryable());
}

TEST(DeliveryResultTest, FromTransportQueueFull) {
    auto dr = DeliveryResult::from_transport(TransportSendResult::QueueFull,
                                             ActorAddress{}, {});
    EXPECT_EQ(dr.status, DeliveryStatus::RemoteUnavailable);
    EXPECT_TRUE(dr.retryable());
}

TEST(DeliveryResultTest, FromTransportCircuitOpen) {
    auto dr = DeliveryResult::from_transport(TransportSendResult::CircuitOpen,
                                             ActorAddress{}, {});
    EXPECT_EQ(dr.status, DeliveryStatus::RemoteUnavailable);
    EXPECT_TRUE(dr.retryable()); // circuit breakers recover after cooldown
}

TEST(DeliveryResultTest, FromTransportEncodeError) {
    auto dr = DeliveryResult::from_transport(TransportSendResult::EncodeError,
                                             ActorAddress{}, {});
    EXPECT_EQ(dr.status, DeliveryStatus::SerializationError);
    EXPECT_FALSE(dr.retryable());
}

TEST(DeliveryResultTest, FromTransportShuttingDown) {
    auto dr = DeliveryResult::from_transport(TransportSendResult::ShuttingDown,
                                             ActorAddress{}, {});
    EXPECT_EQ(dr.status, DeliveryStatus::ShuttingDown);
    EXPECT_FALSE(dr.retryable());
}

TEST(DeliveryResultTest, FromTransportWriteError) {
    auto dr = DeliveryResult::from_transport(TransportSendResult::WriteError,
                                             ActorAddress{}, {});
    EXPECT_EQ(dr.status, DeliveryStatus::TransportError);
    EXPECT_TRUE(dr.retryable());
}

} // namespace
} // namespace hpactor::mailbox
