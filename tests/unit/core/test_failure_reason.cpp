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
#include <hpactor/types/failure_reason.hpp>
#include <hpactor/types/types.hpp>

#include <cstring>

// ── retryable() ─────────────────────────────────────────────────────
TEST(FailureReasonTest, Retryable) {
    EXPECT_TRUE(hpactor::retryable(hpactor::FailureReason::NoRoute));
    EXPECT_TRUE(hpactor::retryable(hpactor::FailureReason::NodeUnavailable));
    EXPECT_FALSE(hpactor::retryable(hpactor::FailureReason::ActorDead));
    EXPECT_TRUE(hpactor::retryable(hpactor::FailureReason::ActorNotReady));
    EXPECT_FALSE(hpactor::retryable(hpactor::FailureReason::Quarantined));
    EXPECT_TRUE(hpactor::retryable(hpactor::FailureReason::CircuitOpen));
    EXPECT_TRUE(hpactor::retryable(hpactor::FailureReason::MailboxFull));
    EXPECT_TRUE(hpactor::retryable(hpactor::FailureReason::OutboundQueueFull));
    EXPECT_TRUE(hpactor::retryable(hpactor::FailureReason::MemoryPressure));
    EXPECT_FALSE(hpactor::retryable(hpactor::FailureReason::Expired));
    EXPECT_TRUE(hpactor::retryable(hpactor::FailureReason::Timeout));
    EXPECT_FALSE(hpactor::retryable(hpactor::FailureReason::RejectedByPolicy));
    EXPECT_FALSE(hpactor::retryable(hpactor::FailureReason::Dropped));
    EXPECT_FALSE(hpactor::retryable(hpactor::FailureReason::MailboxClosed));
    EXPECT_FALSE(hpactor::retryable(hpactor::FailureReason::SerializationError));
    EXPECT_TRUE(hpactor::retryable(hpactor::FailureReason::TransportError));
    EXPECT_FALSE(hpactor::retryable(hpactor::FailureReason::FrameRejected));
    EXPECT_FALSE(hpactor::retryable(hpactor::FailureReason::Duplicate));
    EXPECT_TRUE(hpactor::retryable(hpactor::FailureReason::Draining));
    EXPECT_TRUE(hpactor::retryable(hpactor::FailureReason::ShuttingDown));
    EXPECT_FALSE(hpactor::retryable(hpactor::FailureReason::RetryExhausted));
    EXPECT_FALSE(hpactor::retryable(hpactor::FailureReason::SpawnFailed));
    EXPECT_FALSE(hpactor::retryable(hpactor::FailureReason::Unknown));
}

// ── to_string(FailureReason) ────────────────────────────────────────
TEST(FailureReasonTest, ToStringFailureReason) {
    EXPECT_STREQ(hpactor::to_string(hpactor::FailureReason::NoRoute), "no_"
                                                                      "route");
    EXPECT_STREQ(hpactor::to_string(hpactor::FailureReason::ActorDead), "actor_"
                                                                        "dead");
    EXPECT_STREQ(hpactor::to_string(hpactor::FailureReason::MailboxFull), "mail"
                                                                          "box_"
                                                                          "ful"
                                                                          "l");
    EXPECT_STREQ(hpactor::to_string(hpactor::FailureReason::Timeout), "timeou"
                                                                      "t");
    EXPECT_STREQ(hpactor::to_string(hpactor::FailureReason::Unknown), "unknow"
                                                                      "n");

    // to_string returns non-null for all enum values up to 90
    for (uint8_t i = 0; i <= 90; ++i) {
        auto r = static_cast<hpactor::FailureReason>(i);
        const char* s = hpactor::to_string(r);
        ASSERT_NE(s, nullptr);
        EXPECT_GT(std::strlen(s), 0u);
    }
}

// ── to_string(FailureSource) ────────────────────────────────────────
TEST(FailureReasonTest, ToStringFailureSource) {
    EXPECT_STREQ(hpactor::to_string(hpactor::FailureSource::ActorRuntime), "act"
                                                                           "or_"
                                                                           "run"
                                                                           "tim"
                                                                           "e");
    EXPECT_STREQ(hpactor::to_string(hpactor::FailureSource::Mailbox), "mailbo"
                                                                      "x");
    EXPECT_STREQ(hpactor::to_string(hpactor::FailureSource::Unknown), "unknow"
                                                                      "n");
}

// ── EnqueueResultCode -> FailureReason mapping ──────────────────────
TEST(FailureReasonTest, EnqueueResultCodeToFailureReason) {
    using namespace hpactor::mailbox;
    EXPECT_EQ(failure_reason(EnqueueResultCode::Accepted),
              hpactor::FailureReason::Unknown);
    EXPECT_EQ(failure_reason(EnqueueResultCode::AcceptedWithSoftPressure),
              hpactor::FailureReason::Unknown);
    EXPECT_EQ(failure_reason(EnqueueResultCode::Rejected),
              hpactor::FailureReason::RejectedByPolicy);
    EXPECT_EQ(failure_reason(EnqueueResultCode::DroppedNewest),
              hpactor::FailureReason::Dropped);
    EXPECT_EQ(failure_reason(EnqueueResultCode::DroppedExisting),
              hpactor::FailureReason::Dropped);
    EXPECT_EQ(failure_reason(EnqueueResultCode::ReroutedToDeadLetter),
              hpactor::FailureReason::RejectedByPolicy);
    EXPECT_EQ(failure_reason(EnqueueResultCode::ReroutedToOverflow),
              hpactor::FailureReason::RejectedByPolicy);
    EXPECT_EQ(failure_reason(EnqueueResultCode::MailboxClosed),
              hpactor::FailureReason::MailboxClosed);
    EXPECT_EQ(failure_reason(EnqueueResultCode::ActorNotFound),
              hpactor::FailureReason::NoRoute);
}

// ── EnqueueResult::failure_reason() method ──────────────────────────
TEST(FailureReasonTest, EnqueueResultFailureReasonMethod) {
    using namespace hpactor::mailbox;
    {
        EnqueueResult r;
        r.code = EnqueueResultCode::ActorNotFound;
        EXPECT_EQ(r.failure_reason(), hpactor::FailureReason::NoRoute);
    }
    {
        EnqueueResult r;
        r.code = EnqueueResultCode::Accepted;
        EXPECT_EQ(r.failure_reason(), hpactor::FailureReason::Unknown);
    }
}

// ── error::failure_reason() mapping ─────────────────────────────────
TEST(FailureReasonTest, ErrorFailureReasonMapping) {
    {
        hpactor::error err(hpactor::errors::actor_down, "down");
        EXPECT_EQ(err.failure_reason(), hpactor::FailureReason::ActorDead);
    }
    {
        hpactor::error err(hpactor::errors::actor_not_found, "missing");
        EXPECT_EQ(err.failure_reason(), hpactor::FailureReason::NoRoute);
    }
    {
        hpactor::error err(hpactor::errors::mailbox_full, "full");
        EXPECT_EQ(err.failure_reason(), hpactor::FailureReason::MailboxFull);
    }
    {
        hpactor::error err(hpactor::errors::timeout, "timeout");
        EXPECT_EQ(err.failure_reason(), hpactor::FailureReason::Timeout);
    }
    {
        hpactor::error err(hpactor::errors::invalid_argument, "bad arg");
        EXPECT_EQ(err.failure_reason(), hpactor::FailureReason::RejectedByPolicy);
    }
    {
        hpactor::error err(hpactor::errors::unknown, "?");
        EXPECT_EQ(err.failure_reason(), hpactor::FailureReason::Unknown);
    }
    {
        hpactor::error err(9999, "unmapped");
        EXPECT_EQ(err.failure_reason(), hpactor::FailureReason::Unknown);
    }
}
