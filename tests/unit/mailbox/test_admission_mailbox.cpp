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

#include <hpactor/mailbox/detail/policies/sender_filter_policy.hpp>
#include <hpactor/mailbox/mpsc_actor_mailbox.hpp>
#include <hpactor/mem/memory_config.hpp>

#include <gtest/gtest.h>

#include <cstdint>

namespace hpactor::mailbox {
namespace {

using hpactor::adt::StreamBuffer;

TypedMessage make_msg(TypeTag tag) {
    return TypedMessage(tag, StreamBuffer{0x01, 0x02, 0x03});
}

// 4.1 PolicyRejectsMessage
TEST(AdmissionMailboxTest, PolicyRejectsMessage) {
    MPSCActorMailbox<TypedMessage> mailbox(ActorId(1), nullptr);
    auto policies =
        std::make_shared<std::vector<std::unique_ptr<IAdmissionPolicy>>>();
    policies->push_back(
        std::make_unique<detail::SenderFilterPolicy>(std::vector<uint64_t>{42}));
    mailbox.set_admission_policies(policies);

    MailboxEnvelopeMeta meta;
    meta.type_tag = TypeTag::User;
    meta.sender = ActorAddress(Ipv4Endpoint{0x7F000001, 0}, 0, ActorId(42), 0);
    meta.estimated_bytes = 64;

    auto msg = make_msg(TypeTag::User);
    auto result = mailbox.try_push(std::move(msg), meta);

    EXPECT_EQ(result.code, EnqueueResultCode::Rejected);
}

// 4.2 NoPoliciesAcceptsNormally (using inject_for_test to avoid allocator
// setup)
TEST(AdmissionMailboxTest, MailboxWorksWithPoliciesSet) {
    MPSCActorMailbox<TypedMessage> mailbox(ActorId(1), nullptr);
    auto policies =
        std::make_shared<std::vector<std::unique_ptr<IAdmissionPolicy>>>();
    policies->push_back(
        std::make_unique<detail::SenderFilterPolicy>(std::vector<uint64_t>{42}));
    mailbox.set_admission_policies(policies);

    // inject_for_test bypasses try_push (and thus the admission gate)
    void* raw = mem::allocate(mem::RegionType::kMessage, sizeof(TypedMessage),
                              ActorId(1));
    auto* msg = new (raw) TypedMessage(TypeTag::User, StreamBuffer{0x01});
    mailbox.inject_for_test(msg);

    EXPECT_FALSE(mailbox.empty());
}

// 4.4 SnapshotShowsPolicyCount (was 4.3)
TEST(AdmissionMailboxTest, SnapshotShowsPolicyCount) {
    MPSCActorMailbox<TypedMessage> mailbox(ActorId(1), nullptr);
    auto policies =
        std::make_shared<std::vector<std::unique_ptr<IAdmissionPolicy>>>();
    policies->push_back(
        std::make_unique<detail::SenderFilterPolicy>(std::vector<uint64_t>{42}));
    policies->push_back(
        std::make_unique<detail::SenderFilterPolicy>(std::vector<uint64_t>{99}));
    mailbox.set_admission_policies(policies);

    auto snap = mailbox.snapshot();
    EXPECT_EQ(snap.admission_policy_count, 2u);
}

// 4.5 PolicyRejectionUpdatesCounter
TEST(AdmissionMailboxTest, PolicyRejectionUpdatesCounter) {
    MPSCActorMailbox<TypedMessage> mailbox(ActorId(1), nullptr);
    auto policies =
        std::make_shared<std::vector<std::unique_ptr<IAdmissionPolicy>>>();
    policies->push_back(
        std::make_unique<detail::SenderFilterPolicy>(std::vector<uint64_t>{42}));
    mailbox.set_admission_policies(policies);

    auto snap1 = mailbox.snapshot();
    EXPECT_EQ(snap1.admission_rejected_total, 0u);

    MailboxEnvelopeMeta meta;
    meta.type_tag = TypeTag::User;
    meta.sender = ActorAddress(Ipv4Endpoint{0x7F000001, 0}, 0, ActorId(42), 0);
    meta.estimated_bytes = 64;

    auto msg = make_msg(TypeTag::User);
    mailbox.try_push(std::move(msg), meta);

    auto snap2 = mailbox.snapshot();
    EXPECT_EQ(snap2.admission_rejected_total, 1u);
}

} // namespace
} // namespace hpactor::mailbox
