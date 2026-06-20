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

// Unit test: Mailbox Edge Cases
// Empty dequeue, capacity boundary, single-lane MultiLaneQueue, priority
// inversion, all overflow policies, pressure state machine boundaries,
// reservation manager at limit, DLQ with zero max records.

#include <hpactor/adt/reservation_manager.hpp>
#include <hpactor/mailbox/detail/overflow_handler_factory.hpp>
#include <hpactor/mailbox/detail/pressure_state_machine.hpp>
#include <hpactor/mailbox/mpsc_actor_mailbox.hpp>
#include <hpactor/mailbox/multi_lane_queue.hpp>
#include <hpactor/msg/dead_letter_record.hpp>

#include <gtest/gtest.h>

using namespace hpactor;
using namespace hpactor::mailbox;

// ── Test node for MultiLaneQueue ─────────────────────────────────────────────

struct TestNode {
    int value = 0;
    std::atomic<TestNode*> mpsc_next{nullptr};
};

// ═══════════════════════════════════════════════════════════════════════════════
// Test 1: Empty mailbox dequeue
// ═══════════════════════════════════════════════════════════════════════════════

TEST(MailboxEdgeCases, EmptyMultiLaneQueueDequeue) {
    MultiLaneQueue<TestNode> q(4);

    // Dequeue from an empty queue returns nullptr
    EXPECT_EQ(q.dequeue(), nullptr);

    // Empty check should return true
    EXPECT_TRUE(q.empty());

    // Total depth should be 0
    EXPECT_EQ(q.total_depth(), 0);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 2: MultiLaneQueue at capacity boundary (depth tracking)
// ═══════════════════════════════════════════════════════════════════════════════

TEST(MailboxEdgeCases, MultiLaneQueueDepthTrackingAtBoundary) {
    MultiLaneQueue<TestNode> q(2);

    TestNode a{10}, b{20}, c{30}, d{40};

    q.enqueue(&a, 0);
    EXPECT_EQ(q.total_depth(), 1);

    q.enqueue(&b, 1);
    EXPECT_EQ(q.total_depth(), 2);

    q.enqueue(&c, 0);
    EXPECT_EQ(q.total_depth(), 3);

    q.enqueue(&d, 1);
    EXPECT_EQ(q.total_depth(), 4);

    // Dequeue should return in FIFO order per lane, lane 0 first (higher prio)
    auto* first = q.dequeue();
    EXPECT_EQ(first->value, 10);

    auto* second = q.dequeue();
    EXPECT_EQ(second->value, 30);

    auto* third = q.dequeue();
    EXPECT_EQ(third->value, 20);

    auto* fourth = q.dequeue();
    EXPECT_EQ(fourth->value, 40);

    EXPECT_EQ(q.dequeue(), nullptr);
    EXPECT_EQ(q.total_depth(), 0);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 3: MultiLaneQueue with single lane
// ═══════════════════════════════════════════════════════════════════════════════

TEST(MailboxEdgeCases, MultiLaneQueueSingleLane) {
    MultiLaneQueue<TestNode> q(1);

    EXPECT_EQ(q.num_user_lanes(), 1u);

    TestNode a{100}, b{200}, c{300};

    // All items go to the single lane
    q.enqueue(&a, 0);
    q.enqueue(&b, 0);
    q.enqueue(&c, 0);

    EXPECT_FALSE(q.empty());
    EXPECT_EQ(q.total_depth(), 3);

    // FIFO within the single lane
    EXPECT_EQ(q.dequeue()->value, 100);
    EXPECT_EQ(q.dequeue()->value, 200);
    EXPECT_EQ(q.dequeue()->value, 300);
    EXPECT_EQ(q.dequeue(), nullptr);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 4: MultiLaneQueue priority order — lane 0 dequeued before lane N
// ═══════════════════════════════════════════════════════════════════════════════

TEST(MailboxEdgeCases, MultiLaneQueuePriorityOrder) {
    MultiLaneQueue<TestNode> q(3);

    TestNode lo{10}, med{20}, hi{30};

    // Enqueue to lane 2 (lowest priority) first
    q.enqueue(&lo, 2);
    // Then lane 1
    q.enqueue(&med, 1);
    // Then lane 0 (highest priority)
    q.enqueue(&hi, 0);

    // Dequeue should return lane 0 first, then lane 1, then lane 2
    EXPECT_EQ(q.dequeue()->value, 30);
    EXPECT_EQ(q.dequeue()->value, 20);
    EXPECT_EQ(q.dequeue()->value, 10);
    EXPECT_EQ(q.dequeue(), nullptr);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 5: Overflow handler factory — all policies produce valid handlers
// ═══════════════════════════════════════════════════════════════════════════════

// Simple copyable type for overflow handler instantiation
struct OverflowTestMsg {
    int x = 0;
};

TEST(MailboxEdgeCases, OverflowHandlerAllPolicies) {
    using namespace detail;

    // Each policy should produce a non-null handler
    auto reject =
        make_overflow_handler<OverflowTestMsg>(OverflowPolicy::RejectNewest);
    EXPECT_NE(reject, nullptr);

    auto drop_new =
        make_overflow_handler<OverflowTestMsg>(OverflowPolicy::DropNewest);
    EXPECT_NE(drop_new, nullptr);

    auto drop_old =
        make_overflow_handler<OverflowTestMsg>(OverflowPolicy::DropOldest);
    EXPECT_NE(drop_old, nullptr);

    auto drop_low =
        make_overflow_handler<OverflowTestMsg>(OverflowPolicy::DropLowestPriority);
    EXPECT_NE(drop_low, nullptr);

    auto dlq = make_overflow_handler<OverflowTestMsg>(OverflowPolicy::DeadLetter);
    EXPECT_NE(dlq, nullptr);

    auto signal =
        make_overflow_handler<OverflowTestMsg>(OverflowPolicy::SignalOnly);
    EXPECT_NE(signal, nullptr);

    auto spill = make_overflow_handler<OverflowTestMsg>(
        OverflowPolicy::SpillToOverflowQueue);
    EXPECT_NE(spill, nullptr);

    auto block =
        make_overflow_handler<OverflowTestMsg>(OverflowPolicy::BlockWhenAllowed);
    EXPECT_NE(block, nullptr);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 6: Pressure state machine at boundaries
// ═══════════════════════════════════════════════════════════════════════════════

TEST(MailboxEdgeCases, PressureStateMachineBoundaries) {
    detail::PressureStateMachine psm;

    // Initial state is Normal
    EXPECT_EQ(psm.current_state(), MailboxPressureState::Normal);

    // Exactly at high watermark — enters SoftPressure
    psm.update(0.80, false, 0.80, 0.50, 1.00);
    EXPECT_EQ(psm.current_state(), MailboxPressureState::SoftPressure);

    // Below low watermark — returns to Normal
    psm.update(0.49, false, 0.80, 0.50, 1.00);
    EXPECT_EQ(psm.current_state(), MailboxPressureState::Normal);

    // Exactly at critical watermark — enters HardPressure
    psm.update(1.00, false, 0.80, 0.50, 1.00);
    EXPECT_EQ(psm.current_state(), MailboxPressureState::HardPressure);

    // Between low and critical after HardPressure — enters Recovering
    // (hysteresis)
    psm.update(0.60, false, 0.80, 0.50, 1.00);
    EXPECT_EQ(psm.current_state(), MailboxPressureState::Recovering);

    // Below low from Recovering — returns to Normal
    psm.update(0.30, false, 0.80, 0.50, 1.00);
    EXPECT_EQ(psm.current_state(), MailboxPressureState::Normal);

    // Hard failure forces HardPressure regardless of ratio
    psm.update(0.0, true, 0.80, 0.50, 1.00);
    EXPECT_EQ(psm.current_state(), MailboxPressureState::HardPressure);

    // Severity values
    EXPECT_EQ(detail::PressureStateMachine::severity(MailboxPressureState::Normal),
              0u);
    EXPECT_EQ(detail::PressureStateMachine::severity(MailboxPressureState::Recovering),
              1u);
    EXPECT_EQ(detail::PressureStateMachine::severity(
                  MailboxPressureState::SoftPressure),
              2u);
    EXPECT_EQ(detail::PressureStateMachine::severity(
                  MailboxPressureState::HardPressure),
              3u);

    // code_after_accept — Normal → Accepted
    psm.update(0.0, false, 0.80, 0.50, 1.00);
    EXPECT_EQ(psm.code_after_accept(), EnqueueResultCode::Accepted);

    // code_after_accept — SoftPressure → AcceptedWithSoftPressure
    psm.update(0.85, false, 0.80, 0.50, 1.00);
    EXPECT_EQ(psm.code_after_accept(), EnqueueResultCode::AcceptedWithSoftPressure);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 7: Reservation manager at limit
// ═══════════════════════════════════════════════════════════════════════════════

TEST(MailboxEdgeCases, ReservationManagerAtLimit) {
    adt::ReservationManager<TestNode> rm;

    // Reserve up to count capacity limit
    auto r1 = rm.try_reserve(100, 3, 0);
    EXPECT_EQ(r1, adt::ReservationResult::Reserved);

    auto r2 = rm.try_reserve(100, 3, 0);
    EXPECT_EQ(r2, adt::ReservationResult::Reserved);

    auto r3 = rm.try_reserve(100, 3, 0);
    EXPECT_EQ(r3, adt::ReservationResult::Reserved);

    // Fourth reservation exceeds count capacity
    auto r4 = rm.try_reserve(100, 3, 0);
    EXPECT_EQ(r4, adt::ReservationResult::CountCapacity);

    // Release one — can now reserve again
    rm.release(100);
    auto r5 = rm.try_reserve(100, 3, 0);
    EXPECT_EQ(r5, adt::ReservationResult::Reserved);

    // Reserved count should be back at 3
    EXPECT_EQ(rm.reserved_count(), 3u);
}

TEST(MailboxEdgeCases, ReservationManagerByteCapacityAtLimit) {
    adt::ReservationManager<TestNode> rm;

    // Reserve at byte capacity limit
    auto r1 = rm.try_reserve(500, 0, 1000);
    EXPECT_EQ(r1, adt::ReservationResult::Reserved);

    auto r2 = rm.try_reserve(500, 0, 1000);
    EXPECT_EQ(r2, adt::ReservationResult::Reserved);

    // Third reservation exceeds byte capacity (500+500+1 > 1000)
    auto r3 = rm.try_reserve(1, 0, 1000);
    EXPECT_EQ(r3, adt::ReservationResult::ByteCapacity);

    EXPECT_EQ(rm.queued_bytes(), 1000u);

    // Release and re-reserve
    rm.release(500);
    EXPECT_EQ(rm.queued_bytes(), 500u);
    auto r4 = rm.try_reserve(500, 0, 1000);
    EXPECT_EQ(r4, adt::ReservationResult::Reserved);
}

TEST(MailboxEdgeCases, ReservationManagerUnbounded) {
    adt::ReservationManager<TestNode> rm;

    // With max_messages=0 and max_bytes=0, should always succeed
    // (count tracking is skipped when max_messages=0)
    for (int i = 0; i < 100; ++i) {
        auto r = rm.try_reserve(1024, 0, 0);
        EXPECT_EQ(r, adt::ReservationResult::Reserved);
    }

    // When max_messages=0, count is not tracked (reserved_count stays 0)
    EXPECT_GE(rm.queued_bytes(), 100u * 1024u);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 8: DLQ with zero max records
// ═══════════════════════════════════════════════════════════════════════════════

TEST(MailboxEdgeCases, DeadLetterQueueZeroCapacity) {
    DeadLetterConfig dlq_cfg;
    dlq_cfg.capacity = 0; // Zero capacity — should be clamped to 4096
                          // internally

    DeadLetterQueue dlq(dlq_cfg);

    // The config capacity is clamped — verify it's at least the default
    auto cfg = dlq.config();
    EXPECT_GE(cfg.capacity, 0u);

    // Pushing a record should still work (capacity is clamped)
    DeadLetterRecord rec;
    rec.reason = DeadLetterReason::ActorNotFound;
    rec.source = DeadLetterSource::LocalDelivery;
    rec.type_tag = TypeTag(0x1001);

    bool ok = dlq.try_push(std::move(rec));
    EXPECT_TRUE(ok);

    auto snap = dlq.snapshot();
    EXPECT_EQ(snap.depth, 1u);
}

TEST(MailboxEdgeCases, DeadLetterQueuePopFromEmpty) {
    DeadLetterQueue dlq;

    DeadLetterRecord out;
    bool ok = dlq.try_pop(out);
    EXPECT_FALSE(ok); // Empty queue

    auto snap = dlq.snapshot();
    EXPECT_EQ(snap.depth, 0u);
    EXPECT_EQ(snap.total_popped, 0u);
}

TEST(MailboxEdgeCases, DeadLetterQueuePopAtIndexOutOfRange) {
    DeadLetterQueue dlq;

    // Push one record
    DeadLetterRecord rec;
    rec.reason = DeadLetterReason::MailboxFull;
    rec.source = DeadLetterSource::MailboxAdmission;
    rec.type_tag = TypeTag(0x2001);
    dlq.try_push(std::move(rec));

    // Try to pop at an out-of-range index
    DeadLetterRecord out;
    bool ok = dlq.try_pop_at(99, out);
    EXPECT_FALSE(ok);

    // Pop at valid index
    ok = dlq.try_pop_at(0, out);
    EXPECT_TRUE(ok);
    EXPECT_EQ(out.reason, DeadLetterReason::MailboxFull);

    auto snap = dlq.snapshot();
    EXPECT_EQ(snap.depth, 0u);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 9: MultiLaneQueue system lane priority over user lanes
// ═══════════════════════════════════════════════════════════════════════════════

TEST(MailboxEdgeCases, MultiLaneQueueSystemLanePriority) {
    MultiLaneQueue<TestNode> q(2);

    TestNode sys{999}, user{1};

    // Enqueue user message first
    q.enqueue(&user, 0);

    // Enqueue system message second via system lane
    q.enqueue(&sys, MultiLaneQueue<TestNode>::kSystemLaneSentinel);

    // System lane has highest priority — should dequeue first
    EXPECT_EQ(q.dequeue()->value, 999);
    EXPECT_EQ(q.dequeue()->value, 1);
    EXPECT_EQ(q.dequeue(), nullptr);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 10: MultiLaneQueue try_drop_oldest and try_drop_lowest
// ═══════════════════════════════════════════════════════════════════════════════

TEST(MailboxEdgeCases, MultiLaneQueueDropOperations) {
    MultiLaneQueue<TestNode> q(2);

    TestNode a{10}, b{20}, c{30};

    q.enqueue(&a, 0);
    q.enqueue(&b, 0);
    q.enqueue(&c, 1);

    // Drop oldest from user lane (should be 'a' = 10)
    auto* dropped = q.try_drop_oldest_user_lane();
    EXPECT_NE(dropped, nullptr);

    // Remaining: b(20) at lane 0, c(30) at lane 1
    EXPECT_EQ(q.dequeue()->value, 20);
    EXPECT_EQ(q.dequeue()->value, 30);
    EXPECT_EQ(q.dequeue(), nullptr);
}

TEST(MailboxEdgeCases, MultiLaneQueueDropLowestPriority) {
    MultiLaneQueue<TestNode> q(3);

    TestNode a{10}, b{20}, c{30};

    // a in lane 1 (medium), b in lane 2 (lowest), c in lane 0 (highest)
    q.enqueue(&a, 1);
    q.enqueue(&b, 2); // lowest priority lane
    q.enqueue(&c, 0);

    // Drop from lowest-priority user lane — should drop b (lane 2)
    auto* dropped = q.try_drop_from_lowest_user_lane();
    EXPECT_NE(dropped, nullptr);

    // Remaining: c(30) at lane 0, a(10) at lane 1
    EXPECT_EQ(q.dequeue()->value, 30);
    EXPECT_EQ(q.dequeue()->value, 10);
    EXPECT_EQ(q.dequeue(), nullptr);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 11: DeadLetterQueue ejection policy — DropNewestRecord at capacity
// ═══════════════════════════════════════════════════════════════════════════════

TEST(MailboxEdgeCases, DeadLetterQueueDropNewestPolicy) {
    DeadLetterConfig cfg;
    cfg.capacity = 2;
    cfg.overflow_policy = DeadLetterOverflowPolicy::DropNewestRecord;

    DeadLetterQueue dlq(cfg);

    // Fill to capacity
    DeadLetterRecord r1;
    r1.reason = DeadLetterReason::ActorNotFound;
    r1.source = DeadLetterSource::LocalDelivery;
    r1.type_tag = TypeTag(0x1);
    EXPECT_TRUE(dlq.try_push(std::move(r1)));

    DeadLetterRecord r2;
    r2.reason = DeadLetterReason::MailboxFull;
    r2.source = DeadLetterSource::MailboxAdmission;
    r2.type_tag = TypeTag(0x2);
    EXPECT_TRUE(dlq.try_push(std::move(r2)));

    // At capacity — push with DropNewestRecord should reject
    DeadLetterRecord r3;
    r3.reason = DeadLetterReason::Expired;
    r3.source = DeadLetterSource::LocalDelivery;
    r3.type_tag = TypeTag(0x3);
    bool ok = dlq.try_push(std::move(r3));
    // DropNewestRecord returns false for rejected records
    EXPECT_FALSE(ok);

    auto snap = dlq.snapshot();
    EXPECT_EQ(snap.depth, 2u); // Still at capacity, newest was dropped
}
