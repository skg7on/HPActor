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

#include <hpactor/mailbox/detail/policies/per_sender_rate_policy.hpp>
#include <hpactor/mailbox/detail/policies/priority_threshold_policy.hpp>
#include <hpactor/mailbox/detail/policies/sender_filter_policy.hpp>
#include <hpactor/mailbox/detail/policies/size_limit_policy.hpp>
#include <hpactor/mailbox/detail/policies/type_filter_policy.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>

namespace hpactor::mailbox::detail {
namespace {

using hpactor::adt::StreamBuffer;

TypedMessage make_msg(TypeTag tag) {
    return TypedMessage(tag, StreamBuffer{0x01, 0x02, 0x03});
}

ActorAddress make_addr(ActorId id) {
    return {Ipv4Endpoint{0x7F000001, 0}, 0, id, 0};
}

MailboxEnvelopeMeta make_meta(TypeTag tag, ActorId sender = ActorId(0),
                              uint8_t priority = 0, uint64_t bytes = 0) {
    MailboxEnvelopeMeta meta;
    meta.type_tag = tag;
    meta.sender = make_addr(sender);
    meta.priority = priority;
    meta.estimated_bytes = bytes;
    return meta;
}

// 3.1 TypeFilterRejectsBlockedTag
TEST(AdmissionPolicyTest, TypeFilterRejectsBlockedTag) {
    TypeFilterPolicy policy({}, {42});
    auto msg = make_msg(static_cast<TypeTag>(42));
    auto meta = make_meta(static_cast<TypeTag>(42));
    auto result = policy.evaluate(msg, meta, {}, 0);
    EXPECT_EQ(result.decision, AdmissionDecision::Reject);
}

// 3.2 TypeFilterAcceptsAllowedTag
TEST(AdmissionPolicyTest, TypeFilterAcceptsAllowedTag) {
    TypeFilterPolicy policy({42}, {});
    auto msg = make_msg(static_cast<TypeTag>(42));
    auto meta = make_meta(static_cast<TypeTag>(42));
    auto result = policy.evaluate(msg, meta, {}, 0);
    EXPECT_EQ(result.decision, AdmissionDecision::Accept);
}

// 3.3 TypeFilterRejectsNotInAllowedSet
TEST(AdmissionPolicyTest, TypeFilterRejectsNotInAllowedSet) {
    TypeFilterPolicy policy({42}, {});
    auto msg = make_msg(static_cast<TypeTag>(99));
    auto meta = make_meta(static_cast<TypeTag>(99));
    auto result = policy.evaluate(msg, meta, {}, 0);
    EXPECT_EQ(result.decision, AdmissionDecision::Reject);
}

// 3.4 TypeFilterEmptyAllowedBlockedAcceptsAll
TEST(AdmissionPolicyTest, TypeFilterEmptyAllowedBlockedAcceptsAll) {
    TypeFilterPolicy policy({}, {});
    auto msg = make_msg(TypeTag::User);
    auto meta = make_meta(TypeTag::User);
    auto result = policy.evaluate(msg, meta, {}, 0);
    EXPECT_EQ(result.decision, AdmissionDecision::Accept);
}

// 3.5 SenderFilterRejectsBlockedSender
TEST(AdmissionPolicyTest, SenderFilterRejectsBlockedSender) {
    SenderFilterPolicy policy({42});
    auto msg = make_msg(TypeTag::User);
    auto meta = make_meta(TypeTag::User, ActorId(42));
    auto result = policy.evaluate(msg, meta, {}, 0);
    EXPECT_EQ(result.decision, AdmissionDecision::Reject);
}

// 3.6 SenderFilterAcceptsNonBlockedSender
TEST(AdmissionPolicyTest, SenderFilterAcceptsNonBlockedSender) {
    SenderFilterPolicy policy({42});
    auto msg = make_msg(TypeTag::User);
    auto meta = make_meta(TypeTag::User, ActorId(99));
    auto result = policy.evaluate(msg, meta, {}, 0);
    EXPECT_EQ(result.decision, AdmissionDecision::Accept);
}

// 3.7 PriorityThresholdRejectsBelow
TEST(AdmissionPolicyTest, PriorityThresholdRejectsBelow) {
    PriorityThresholdPolicy policy(3);
    auto msg = make_msg(TypeTag::User);
    auto meta = make_meta(TypeTag::User, ActorId(0), 1);
    auto result = policy.evaluate(msg, meta, {}, 0);
    EXPECT_EQ(result.decision, AdmissionDecision::Reject);
}

// 3.8 PriorityThresholdAcceptsAtOrAbove
TEST(AdmissionPolicyTest, PriorityThresholdAcceptsAtOrAbove) {
    PriorityThresholdPolicy policy(3);
    auto msg = make_msg(TypeTag::User);

    auto meta3 = make_meta(TypeTag::User, ActorId(0), 3);
    EXPECT_EQ(policy.evaluate(msg, meta3, {}, 0).decision,
              AdmissionDecision::Accept);

    auto meta5 = make_meta(TypeTag::User, ActorId(0), 5);
    EXPECT_EQ(policy.evaluate(msg, meta5, {}, 0).decision,
              AdmissionDecision::Accept);
}

// 3.9 SizeLimitRejectsOversized
TEST(AdmissionPolicyTest, SizeLimitRejectsOversized) {
    SizeLimitPolicy policy(1024);
    auto msg = make_msg(TypeTag::User);
    auto meta = make_meta(TypeTag::User, ActorId(0), 0, 2048);
    auto result = policy.evaluate(msg, meta, {}, 0);
    EXPECT_EQ(result.decision, AdmissionDecision::Reject);
}

// 3.10 SizeLimitAcceptsWithinLimit
TEST(AdmissionPolicyTest, SizeLimitAcceptsWithinLimit) {
    SizeLimitPolicy policy(1024);
    auto msg = make_msg(TypeTag::User);
    auto meta = make_meta(TypeTag::User, ActorId(0), 0, 512);
    auto result = policy.evaluate(msg, meta, {}, 0);
    EXPECT_EQ(result.decision, AdmissionDecision::Accept);
}

// 3.11 PerSenderRateLimitsIndependently
TEST(AdmissionPolicyTest, PerSenderRateLimitsIndependently) {
    PerSenderRatePolicy policy(10.0, 5, 10);
    auto msg = make_msg(TypeTag::User);

    uint32_t a_admitted = 0, b_admitted = 0;
    for (int i = 0; i < 10; i++) {
        auto meta_a = make_meta(TypeTag::User, ActorId(1), 0, 0);
        if (policy.evaluate(msg, meta_a, {}, 0).decision ==
            AdmissionDecision::Accept) {
            a_admitted++;
        }
        auto meta_b = make_meta(TypeTag::User, ActorId(2), 0, 0);
        if (policy.evaluate(msg, meta_b, {}, 0).decision ==
            AdmissionDecision::Accept) {
            b_admitted++;
        }
    }

    EXPECT_LE(a_admitted, 5u);
    EXPECT_LE(b_admitted, 5u);
    EXPECT_GT(a_admitted, 0u);
    EXPECT_GT(b_admitted, 0u);
}

// 3.12 ChainShortCircuitsOnFirstRejection
TEST(AdmissionPolicyTest, ChainShortCircuitsOnFirstRejection) {
    std::vector<std::unique_ptr<IAdmissionPolicy>> chain;
    chain.push_back(std::make_unique<SenderFilterPolicy>(std::vector<uint64_t>{42}));
    chain.push_back(std::make_unique<TypeFilterPolicy>(
        std::vector<uint32_t>{}, std::vector<uint32_t>{99}));

    auto msg = make_msg(static_cast<TypeTag>(99));
    auto meta = make_meta(static_cast<TypeTag>(99), ActorId(42));

    auto result = chain[0]->evaluate(msg, meta, {}, 0);
    EXPECT_EQ(result.decision, AdmissionDecision::Reject);
    EXPECT_STREQ(result.policy_name, "sender_filter");
}

// 3.13 ChainAllAccept
TEST(AdmissionPolicyTest, ChainAllAccept) {
    std::vector<std::unique_ptr<IAdmissionPolicy>> chain;
    chain.push_back(std::make_unique<SenderFilterPolicy>(std::vector<uint64_t>{42}));
    chain.push_back(std::make_unique<TypeFilterPolicy>(
        std::vector<uint32_t>{}, std::vector<uint32_t>{99}));

    auto msg = make_msg(TypeTag::User);
    auto meta = make_meta(TypeTag::User, ActorId(1));

    for (const auto& p : chain) {
        auto result = p->evaluate(msg, meta, {}, 0);
        EXPECT_EQ(result.decision, AdmissionDecision::Accept);
    }
}

// 3.14 PriorityThresholdReroutesToDLQ
TEST(AdmissionPolicyTest, PriorityThresholdReroutesToDLQ) {
    PriorityThresholdPolicy policy(3, true);
    auto msg = make_msg(TypeTag::User);
    auto meta = make_meta(TypeTag::User, ActorId(0), 1);
    auto result = policy.evaluate(msg, meta, {}, 0);
    EXPECT_EQ(result.decision, AdmissionDecision::RerouteToDLQ);
}

} // namespace
} // namespace hpactor::mailbox::detail
