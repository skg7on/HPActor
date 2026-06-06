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
#include <hpactor/msg/enqueue_result.hpp>
#include <hpactor/msg/typed_message.hpp>

using namespace hpactor;
using namespace hpactor::mailbox;

TEST(MailboxPolicyTest, DefaultConfigValues) {
    MailboxConfig cfg;
    (void)cfg;
    EXPECT_EQ(cfg.capacity.max_messages, 1024);
    EXPECT_EQ(cfg.capacity.max_bytes, 0);
    EXPECT_EQ(cfg.overflow_policy, OverflowPolicy::RejectNewest);
    EXPECT_EQ(cfg.high_watermark, 0.80);
    EXPECT_EQ(cfg.low_watermark, 0.50);
    EXPECT_EQ(cfg.protected_system_messages, 32);
    EXPECT_EQ(cfg.max_overflow_depth, 0);
    EXPECT_EQ(cfg.signal_min_interval_ms, 100);
    EXPECT_EQ(cfg.priority_aware, false);
    EXPECT_EQ(cfg.backpressure_mode, BackpressureMode::LocalAndRemoteSignal);
}

TEST(MailboxPolicyTest, EnqueueResultAccepted) {
    EnqueueResult accepted;
    accepted.code = EnqueueResultCode::Accepted;
    EXPECT_TRUE(accepted.accepted());
    EXPECT_FALSE(accepted.retryable());
}

TEST(MailboxPolicyTest, EnqueueResultAcceptedWithSoftPressure) {
    EnqueueResult soft;
    soft.code = EnqueueResultCode::AcceptedWithSoftPressure;
    EXPECT_TRUE(soft.accepted());
}

TEST(MailboxPolicyTest, EnqueueResultRejected) {
    EnqueueResult rejected;
    rejected.code = EnqueueResultCode::Rejected;
    rejected.retry_after = std::chrono::milliseconds{5};
    EXPECT_FALSE(rejected.accepted());
    EXPECT_TRUE(rejected.retryable());
}

TEST(MailboxPolicyTest, EstimateMessageBytes) {
    TypedMessage user_msg(TypeTag::User, StreamBuffer{1, 2, 3, 4});
    EXPECT_GE(estimate_message_bytes(user_msg), sizeof(TypedMessage) + 4);
}

TEST(MailboxPolicyTest, IsSystemMessage) {
    EXPECT_TRUE(is_system_message(TypeTag::DownMsg));
    EXPECT_FALSE(is_system_message(TypeTag::User));
}
