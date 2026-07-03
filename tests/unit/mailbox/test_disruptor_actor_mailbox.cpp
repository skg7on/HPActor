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

// ── Delivery port observability callbacks ──────────────────────────────────

struct ObserveContext {
    int accepted{0};
    int rejected{0};
};

TEST(DisruptorActorMailboxCoreTest, RecordAcceptedCallbackInvoked) {
    auto core = std::make_shared<Core>(ActorId{1}, ActorAddress{});
    ObserveContext ctx;
    DisruptorMailboxDelivery delivery;
    delivery.context = &ctx;
    delivery.record_accepted =
        +[](void* c, ActorId /*actor*/,
            const DisruptorDeliveryObservation& /*obs*/) noexcept {
            static_cast<ObserveContext*>(c)->accepted++;
        };
    core->set_delivery_port(delivery);

    auto result = core->try_push_user(UserA{42}, DisruptorEnvelopeMeta{});
    EXPECT_TRUE(result.accepted());
    EXPECT_EQ(ctx.accepted, 1);
}

TEST(DisruptorActorMailboxCoreTest, RecordRejectedCallbackInvokedOnFull) {
    auto core = std::make_shared<Core>(ActorId{1}, ActorAddress{});
    ObserveContext ctx;
    DisruptorMailboxDelivery delivery;
    delivery.context = &ctx;
    delivery.record_rejected =
        +[](void* c, ActorId /*actor*/,
            const DisruptorDeliveryFailure& /*failure*/) noexcept {
            static_cast<ObserveContext*>(c)->rejected++;
        };
    core->set_delivery_port(delivery);

    // Fill the ring.
    for (int i = 0; i < 8; ++i) {
        core->try_push_user(UserA{static_cast<uint64_t>(i)},
                            DisruptorEnvelopeMeta{});
    }
    auto result = core->try_push_user(UserA{99}, DisruptorEnvelopeMeta{});
    EXPECT_FALSE(result.accepted());
    EXPECT_EQ(ctx.rejected, 1);
}

TEST(DisruptorActorMailboxCoreTest, RecordRejectedCallbackInvokedOnClosed) {
    auto core = std::make_shared<Core>(ActorId{1}, ActorAddress{});
    ObserveContext ctx;
    DisruptorMailboxDelivery delivery;
    delivery.context = &ctx;
    delivery.record_rejected =
        +[](void* c, ActorId /*actor*/,
            const DisruptorDeliveryFailure& /*failure*/) noexcept {
            static_cast<ObserveContext*>(c)->rejected++;
        };
    core->set_delivery_port(delivery);

    core->close();
    auto result = core->try_push_user(UserA{1}, DisruptorEnvelopeMeta{});
    EXPECT_FALSE(result.accepted());
    EXPECT_EQ(ctx.rejected, 1);
}

// ── Snapshot through execution port ──────────────────────────────────────

TEST(DisruptorActorMailboxCoreTest, SnapshotFnReturnsCorrectData) {
    auto core = std::make_shared<Core>(ActorId{1}, ActorAddress{});
    // Push 3 user messages.
    for (int i = 0; i < 3; ++i) {
        core->try_push_user(UserA{static_cast<uint64_t>(i)},
                            DisruptorEnvelopeMeta{});
    }
    auto binding = core->make_handle();
    ASSERT_NE(binding.execution.snapshot_fn, nullptr);

    auto snap = binding.execution.snapshot_fn(binding.execution.context);
    EXPECT_EQ(snap.depth, 3u);
    EXPECT_EQ(snap.capacity, 8u);
    EXPECT_GT(snap.max_depth, 0u);
    EXPECT_EQ(snap.total_enqueued, 3u);
    EXPECT_EQ(snap.num_user_lanes, 1u);
    EXPECT_EQ(snap.lane_depths[0], 3u);
}

// ── Drain immediate through lifecycle port ──────────────────────────────

TEST(DisruptorActorMailboxCoreTest, DrainImmediateClearsBothLanes) {
    auto core = std::make_shared<Core>(ActorId{1}, ActorAddress{});
    // Push user messages.
    core->try_push_user(UserA{1}, DisruptorEnvelopeMeta{});
    core->try_push_user(UserA{2}, DisruptorEnvelopeMeta{});
    // Push a system message.
    TypedMessage sys_msg;
    core->try_push_control(std::move(sys_msg));

    auto binding = core->make_handle();
    ASSERT_NE(binding.lifecycle.drain_immediate, nullptr);

    // Test via the core directly (drain_immediate port wraps drain_all_now).
    core->drain_all_now();

    // Both lanes should be empty.
    EXPECT_TRUE(core->empty());
    // New user push should be rejected (drain began).
    auto result = core->try_push_user(UserA{3}, DisruptorEnvelopeMeta{});
    EXPECT_FALSE(result.accepted());
}

// ── Bounded system lane ──────────────────────────────────────────────────

TEST(DisruptorActorMailboxCoreTest, SystemLaneRespectsBoundedCapacity) {
    // Create core with protected_system_messages = 2.
    auto core =
        std::make_shared<Core>(ActorId{1}, ActorAddress{}, MailboxConfig{}, 2);

    TypedMessage m1, m2, m3;
    EXPECT_TRUE(core->try_push_control(std::move(m1)).accepted());
    EXPECT_TRUE(core->try_push_control(std::move(m2)).accepted());
    auto result = core->try_push_control(std::move(m3));
    EXPECT_FALSE(result.accepted());
    EXPECT_EQ(result.code, EnqueueResultCode::Rejected);
}

// ── Phase 1: ReservationManager bounded admission ────────────────────────

TEST(DisruptorActorMailboxCoreTest, RespectsMaxMessagesLimit) {
    MailboxConfig cfg;
    cfg.capacity.max_messages = 4;
    auto core = std::make_shared<Core>(ActorId{1}, ActorAddress{}, cfg, 32);

    for (int i = 0; i < 4; ++i) {
        auto r = core->try_push_user(UserA{static_cast<uint64_t>(i)},
                                     DisruptorEnvelopeMeta{});
        EXPECT_TRUE(r.accepted()) << "i=" << i;
    }
    // 5th should be rejected by reservation.
    auto r = core->try_push_user(UserA{99}, DisruptorEnvelopeMeta{});
    EXPECT_FALSE(r.accepted());
}

TEST(DisruptorActorMailboxCoreTest, RespectsByteCapacity) {
    MailboxConfig cfg;
    cfg.capacity.max_messages = 1024;
    // Allow only 2 envelopes worth of bytes.
    cfg.capacity.max_bytes = sizeof(Core::envelope_type) * 2;
    auto core = std::make_shared<Core>(ActorId{1}, ActorAddress{}, cfg, 32);

    EXPECT_TRUE(core->try_push_user(UserA{1}, DisruptorEnvelopeMeta{}).accepted());
    EXPECT_TRUE(core->try_push_user(UserA{2}, DisruptorEnvelopeMeta{}).accepted());
    // 3rd exceeds byte capacity.
    EXPECT_FALSE(core->try_push_user(UserA{3}, DisruptorEnvelopeMeta{}).accepted());
}

// ── Phase 2: Pressure state and backpressure signals ────────────────────

TEST(DisruptorActorMailboxCoreTest, PressureStateTransitionsToSoftPressure) {
    MailboxConfig cfg;
    cfg.capacity.max_messages = 8;
    cfg.high_watermark = 0.5; // SoftPressure at >= 50%
    cfg.low_watermark = 0.25;
    auto core = std::make_shared<Core>(ActorId{1}, ActorAddress{}, cfg, 32);

    // Fill to 5/8 = 62.5% — above high watermark.
    for (int i = 0; i < 5; ++i) {
        core->try_push_user(UserA{static_cast<uint64_t>(i)},
                            DisruptorEnvelopeMeta{});
    }
    EXPECT_EQ(core->pressure_state(), MailboxPressureState::SoftPressure);
}

TEST(DisruptorActorMailboxCoreTest, EnqueueResultCarriesPressureFields) {
    MailboxConfig cfg;
    cfg.capacity.max_messages = 8;
    cfg.high_watermark = 0.80;
    auto core = std::make_shared<Core>(ActorId{1}, ActorAddress{}, cfg, 32);

    auto result = core->try_push_user(UserA{42}, DisruptorEnvelopeMeta{});
    EXPECT_TRUE(result.accepted());
    EXPECT_EQ(result.depth, 1u);
    EXPECT_EQ(result.capacity, 8u);
    EXPECT_GT(result.pressure_ratio, 0.0);
}

TEST(DisruptorActorMailboxCoreTest, RejectionUpdatesPressureToHard) {
    MailboxConfig cfg;
    cfg.capacity.max_messages = 4;
    auto core = std::make_shared<Core>(ActorId{1}, ActorAddress{}, cfg, 32);

    // Fill completely.
    for (int i = 0; i < 4; ++i) {
        core->try_push_user(UserA{static_cast<uint64_t>(i)},
                            DisruptorEnvelopeMeta{});
    }
    // Rejection triggers hard_failure update → HardPressure.
    core->try_push_user(UserA{99}, DisruptorEnvelopeMeta{});
    EXPECT_EQ(core->pressure_state(), MailboxPressureState::HardPressure);
}

} // namespace
} // namespace hpactor::mailbox
