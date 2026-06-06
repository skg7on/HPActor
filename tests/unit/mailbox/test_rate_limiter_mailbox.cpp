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
#include <hpactor/mailbox/mpsc_actor_mailbox.hpp>
#include <hpactor/mem/memory_config.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>

namespace hpactor::mailbox {
namespace {

StreamBuffer make_payload() {
    return {0x01, 0x02, 0x03};
}

// 2.1 RateLimitedDequeueReturnsNullptr
TEST(RateLimiterMailboxTest, RateLimitedDequeueReturnsNullptr) {
    MPSCActorMailbox<TypedMessage> mailbox(ActorId(1), nullptr);

    auto limiter = std::make_unique<ActorRateLimiter>();
    limiter->configure(0.1, 0); // 0.1 msg/s, burst 0
    mailbox.set_rate_limiter(std::move(limiter));

    // Allocate message on heap (try_pop will mem::deallocate it)
    void* raw = mem::allocate(mem::RegionType::kMessage, sizeof(TypedMessage),
                              ActorId(1));
    auto* msg = new (raw) TypedMessage(TypeTag::User, make_payload());
    mailbox.inject_for_test(msg);

    TypedMessage out;
    EXPECT_FALSE(mailbox.try_pop(out));
}

// 2.2 RateAdmittedDequeueReturnsMessage
TEST(RateLimiterMailboxTest, RateAdmittedDequeueReturnsMessage) {
    MPSCActorMailbox<TypedMessage> mailbox(ActorId(1), nullptr);

    auto limiter = std::make_unique<ActorRateLimiter>();
    limiter->configure(1000000.0, 1000000);
    mailbox.set_rate_limiter(std::move(limiter));

    void* raw = mem::allocate(mem::RegionType::kMessage, sizeof(TypedMessage),
                              ActorId(1));
    auto* msg = new (raw) TypedMessage(TypeTag::User, make_payload());
    mailbox.inject_for_test(msg);

    TypedMessage out;
    EXPECT_TRUE(mailbox.try_pop(out));
}

// 2.3 SnapshotReflectsRateLimiterState
TEST(RateLimiterMailboxTest, SnapshotReflectsRateLimiterState) {
    MPSCActorMailbox<TypedMessage> mailbox(ActorId(1), nullptr);

    auto limiter = std::make_unique<ActorRateLimiter>();
    limiter->configure(50.0, 10);
    mailbox.set_rate_limiter(std::move(limiter));

    auto snap = mailbox.snapshot();
    EXPECT_TRUE(snap.rate_limiter_enabled);
    EXPECT_DOUBLE_EQ(snap.rate_limiter_rate, 50.0);
    EXPECT_EQ(snap.rate_limiter_burst, 10u);
    EXPECT_NEAR(snap.rate_limiter_current_tokens, 10.0, 0.01);
}

// 2.4 NoRateLimiterSnapshotShowsDisabled
TEST(RateLimiterMailboxTest, NoRateLimiterSnapshotShowsDisabled) {
    MPSCActorMailbox<TypedMessage> mailbox(ActorId(1), nullptr);
    auto snap = mailbox.snapshot();
    EXPECT_FALSE(snap.rate_limiter_enabled);
    EXPECT_EQ(snap.rate_limit_blocked_total, 0u);
}

// 2.5 NoRateLimiterTryPopStillWorks
TEST(RateLimiterMailboxTest, NoRateLimiterTryPopStillWorks) {
    MPSCActorMailbox<TypedMessage> mailbox(ActorId(1), nullptr);

    void* raw = mem::allocate(mem::RegionType::kMessage, sizeof(TypedMessage),
                              ActorId(1));
    auto* msg = new (raw) TypedMessage(TypeTag::User, make_payload());
    mailbox.inject_for_test(msg);

    TypedMessage out;
    EXPECT_TRUE(mailbox.try_pop(out));
}

} // namespace
} // namespace hpactor::mailbox
