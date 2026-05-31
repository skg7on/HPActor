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

#include <hpactor/mailbox/detail/handlers/dead_letter_handler.hpp>
#include <hpactor/mailbox/detail/handlers/drop_newest_handler.hpp>
#include <hpactor/mailbox/detail/handlers/drop_oldest_handler.hpp>
#include <hpactor/mailbox/detail/handlers/reject_newest_handler.hpp>
#include <hpactor/mailbox/detail/handlers/signal_only_handler.hpp>
#include <hpactor/mailbox/detail/handlers/spill_to_overflow_handler.hpp>
#include <hpactor/mailbox/detail/overflow_context.hpp>
#include <hpactor/mailbox/mailbox_policy.hpp>
#include <hpactor/mailbox/overflow_queue.hpp>
#include <hpactor/types/types.hpp>

#include <atomic>
#include <gtest/gtest.h>

using namespace hpactor;
using namespace hpactor::mailbox;
using namespace hpactor::mailbox::detail;

struct TestMsg {
    int payload = 0;
};

class OverflowHandlerTest : public ::testing::Test {
  protected:
    void SetUp() override {
        config_.capacity.max_messages = 100;
        config_.capacity.max_bytes = 1024 * 1024;
    }

    template <typename Handler>
    EnqueueResult invoke(Handler& handler, ReservationResult reason) {
        msg_ = TestMsg{42};
        OverflowContext<TestMsg> ctx{msg_,
                                     meta_,
                                     reservation_,
                                     overflow_queue_,
                                     total_rejected_,
                                     total_dropped_,
                                     total_dead_letters_,
                                     nullptr,
                                     config_,
                                     ActorId{1},
                                     /*current_depth=*/100,
                                     /*current_bytes=*/1024 * 1024,
                                     /*drop_oldest_fn=*/nullptr,
                                     /*dlq=*/nullptr,
                                     /*drop_lowest_priority_fn=*/nullptr};
        return handler.handle(ctx, reason);
    }

    TestMsg msg_;
    MailboxEnvelopeMeta meta_;
    ReservationManager<TestMsg> reservation_;
    OverflowQueue<TestMsg> overflow_queue_;
    std::atomic<uint64_t> total_rejected_{0};
    std::atomic<uint64_t> total_dropped_{0};
    std::atomic<uint64_t> total_dead_letters_{0};
    MailboxConfig config_;
};

TEST_F(OverflowHandlerTest, RejectNewestReturnsRejected) {
    RejectNewestHandler<TestMsg> handler;
    auto r = invoke(handler, ReservationResult::CountCapacity);
    EXPECT_EQ(r.code, EnqueueResultCode::Rejected);
    EXPECT_EQ(total_rejected_.load(), 1);
    EXPECT_EQ(r.pressure_reason, BackpressureReason::HardCapacity);
}

TEST_F(OverflowHandlerTest, RejectNewestByteCapacityReason) {
    RejectNewestHandler<TestMsg> handler;
    auto r = invoke(handler, ReservationResult::ByteCapacity);
    EXPECT_EQ(r.code, EnqueueResultCode::Rejected);
    EXPECT_EQ(r.pressure_reason, BackpressureReason::ByteCapacity);
}

TEST_F(OverflowHandlerTest, DropNewestReturnsDroppedNewest) {
    DropNewestHandler<TestMsg> handler;
    auto r = invoke(handler, ReservationResult::CountCapacity);
    EXPECT_EQ(r.code, EnqueueResultCode::DroppedNewest);
    EXPECT_EQ(total_dropped_.load(), 1);
    EXPECT_EQ(r.pressure_reason, BackpressureReason::OverflowPolicy);
}

TEST_F(OverflowHandlerTest, DeadLetterReturnsReroutedToDeadLetter) {
    DeadLetterHandler<TestMsg> handler;
    auto r = invoke(handler, ReservationResult::CountCapacity);
    EXPECT_EQ(r.code, EnqueueResultCode::ReroutedToDeadLetter);
    EXPECT_EQ(total_dead_letters_.load(), 1);
}

TEST_F(OverflowHandlerTest, SignalOnlyReturnsRejectedWithRetryAfter) {
    config_.signal_min_interval_ms = 200;
    SignalOnlyHandler<TestMsg> handler;
    auto r = invoke(handler, ReservationResult::CountCapacity);
    EXPECT_EQ(r.code, EnqueueResultCode::Rejected);
    EXPECT_EQ(r.retry_after.count(), 200);
    EXPECT_EQ(total_rejected_.load(), 1);
}

TEST_F(OverflowHandlerTest, DropOldestNoCallbackReturnsRejected) {
    DropOldestHandler<TestMsg> handler;
    auto r = invoke(handler, ReservationResult::CountCapacity);
    EXPECT_EQ(r.code, EnqueueResultCode::Rejected);
    EXPECT_EQ(total_rejected_.load(), 1);
}

TEST_F(OverflowHandlerTest, DropOldestWithSuccessCallback) {
    DropOldestHandler<TestMsg> handler;
    TestMsg msg{42};
    OverflowContext<TestMsg> ctx{msg,
                                 meta_,
                                 reservation_,
                                 overflow_queue_,
                                 total_rejected_,
                                 total_dropped_,
                                 total_dead_letters_,
                                 nullptr,
                                 config_,
                                 ActorId{1},
                                 /*current_depth=*/100,
                                 /*current_bytes=*/1024 * 1024,
                                 []() { return true; },
                                 /*dlq=*/nullptr,
                                 /*drop_lowest_priority_fn=*/nullptr};
    auto r = handler.handle(ctx, ReservationResult::CountCapacity);
    EXPECT_EQ(r.code, EnqueueResultCode::DroppedExisting);
}

TEST_F(OverflowHandlerTest, SpillToOverflowSucceedsWhenQueueHasRoom) {
    overflow_queue_.set_max_depth(100);
    SpillToOverflowHandler<TestMsg> handler;
    auto r = invoke(handler, ReservationResult::CountCapacity);
    EXPECT_EQ(r.code, EnqueueResultCode::ReroutedToOverflow);
    EXPECT_FALSE(overflow_queue_.empty());
}

TEST_F(OverflowHandlerTest, EachHandlerReportsCorrectPolicy) {
    EXPECT_EQ(RejectNewestHandler<TestMsg>{}.policy(), OverflowPolicy::RejectNewest);
    EXPECT_EQ(DropNewestHandler<TestMsg>{}.policy(), OverflowPolicy::DropNewest);
    EXPECT_EQ(DropOldestHandler<TestMsg>{}.policy(), OverflowPolicy::DropOldest);
    EXPECT_EQ(DeadLetterHandler<TestMsg>{}.policy(), OverflowPolicy::DeadLetter);
    EXPECT_EQ(SignalOnlyHandler<TestMsg>{}.policy(), OverflowPolicy::SignalOnly);
    EXPECT_EQ(SpillToOverflowHandler<TestMsg>{}.policy(),
              OverflowPolicy::SpillToOverflowQueue);
}
