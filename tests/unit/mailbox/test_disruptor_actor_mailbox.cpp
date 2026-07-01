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

#include <hpactor/mailbox/disruptor_actor_mailbox.hpp>
#include <hpactor/mailbox/disruptor_mailbox_interface.hpp>

#include <gtest/gtest.h>

#include <memory>

namespace hpactor::mailbox {
namespace {

struct UserA {
    uint64_t value;
};
struct UserB {
    int64_t value;
};

static_assert(DisruptorMessage<UserA>);
static_assert(DisruptorMessage<UserB>);

using Core = DisruptorActorMailboxCore<8, UserA, UserB>;

// ── Construction and empty state ──────────────────────────────────────────

TEST(DisruptorActorMailboxCoreTest, ConstructsEmpty) {
    Core core(ActorId{1}, ActorAddress{});
    EXPECT_TRUE(core.empty());
}

TEST(DisruptorActorMailboxCoreTest, RingIsEmptyOnConstruction) {
    Core core(ActorId{1}, ActorAddress{});
    EXPECT_TRUE(core.ring().empty());
    EXPECT_FALSE(core.ring().is_closed());
}

// ── User message FIFO ─────────────────────────────────────────────────────

TEST(DisruptorActorMailboxCoreTest, PushAndConsumeUserMessage) {
    Core core(ActorId{1}, ActorAddress{});

    auto result = core.try_push_user(UserA{42}, DisruptorEnvelopeMeta{});
    EXPECT_TRUE(result.accepted());

    EXPECT_FALSE(core.empty());
    EXPECT_FALSE(core.ring().empty());
}

// ── Full ring rejection ───────────────────────────────────────────────────

TEST(DisruptorActorMailboxCoreTest, RejectsWhenRingFull) {
    Core core(ActorId{1}, ActorAddress{});

    // Fill the ring (capacity 8).
    for (int i = 0; i < 8; ++i) {
        auto result = core.try_push_user(UserA{static_cast<uint64_t>(i)},
                                         DisruptorEnvelopeMeta{});
        EXPECT_TRUE(result.accepted()) << "i=" << i;
    }

    // 9th push should fail.
    auto result = core.try_push_user(UserA{99}, DisruptorEnvelopeMeta{});
    EXPECT_FALSE(result.accepted());
    EXPECT_EQ(result.code, EnqueueResultCode::Rejected);
}

// ── Close semantics ───────────────────────────────────────────────────────

TEST(DisruptorActorMailboxCoreTest, CloseRejectsNewPushes) {
    Core core(ActorId{1}, ActorAddress{});
    core.close();

    auto result = core.try_push_user(UserA{1}, DisruptorEnvelopeMeta{});
    EXPECT_FALSE(result.accepted());
    EXPECT_EQ(result.code, EnqueueResultCode::Rejected);
}

// ── Drain rejects new user messages ───────────────────────────────────────

TEST(DisruptorActorMailboxCoreTest, DrainRejectsNewUserMessages) {
    Core core(ActorId{1}, ActorAddress{});

    core.begin_drain();
    auto result = core.try_push_user(UserA{1}, DisruptorEnvelopeMeta{});
    EXPECT_FALSE(result.accepted());
}

// ── Publisher quiescence ──────────────────────────────────────────────────

TEST(DisruptorActorMailboxCoreTest, PublishersQuiescentAfterPushCompletes) {
    Core core(ActorId{1}, ActorAddress{});

    EXPECT_TRUE(core.publishers_quiescent());

    auto result = core.try_push_user(UserA{1}, DisruptorEnvelopeMeta{});
    EXPECT_TRUE(result.accepted());
    EXPECT_TRUE(core.publishers_quiescent());
}

// ── System control message ────────────────────────────────────────────────

TEST(DisruptorActorMailboxCoreTest, PushControlMessage) {
    Core core(ActorId{1}, ActorAddress{});

    // Create a minimal system-level TypedMessage.
    TypedMessage sys_msg;
    auto result = core.try_push_control(std::move(sys_msg));
    EXPECT_TRUE(result.accepted());
    EXPECT_FALSE(core.empty());
}

// ── Binding ────────────────────────────────────────────────────────────────

TEST(DisruptorActorMailboxCoreTest, MakeBindingProducesValidPorts) {
    auto core = std::make_shared<Core>(ActorId{1}, ActorAddress{});
    auto binding = core->make_handle();
    EXPECT_TRUE(binding.valid());
}

// ── Multiple message types ─────────────────────────────────────────────────

TEST(DisruptorActorMailboxCoreTest, HandlesBothMessageTypes) {
    Core core(ActorId{1}, ActorAddress{});

    auto r1 = core.try_push_user(UserA{10}, DisruptorEnvelopeMeta{});
    EXPECT_TRUE(r1.accepted());

    auto r2 = core.try_push_user(UserB{-5}, DisruptorEnvelopeMeta{});
    EXPECT_TRUE(r2.accepted());

    EXPECT_FALSE(core.empty());
}

} // namespace
} // namespace hpactor::mailbox
