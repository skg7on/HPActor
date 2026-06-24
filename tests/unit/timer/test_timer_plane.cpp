// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

// tests/unit/timer/test_timer_plane.cpp

#include <hpactor/sched/scheduler_interfaces.hpp>
#include <hpactor/timer/timer_command_queue.hpp>
#include <hpactor/timer/timer_plane_shard.hpp>

#include <gtest/gtest.h>
#include <thread>
#include <vector>

namespace hpactor::sched {
namespace {

class TimerCommandQueueTest : public ::testing::Test {
  protected:
    TimerCommandQueue queue_;
};

TEST_F(TimerCommandQueueTest, PushAndDrainSingle) {
    TimerNode node;
    auto cmd = TimerCommand::make_schedule(1'000'000, &node);
    EXPECT_TRUE(queue_.try_push(cmd));
    std::vector<TimerCommand> out;
    EXPECT_EQ(queue_.drain_all(out), 1u);
    EXPECT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].type, TimerCommand::Type::Schedule);
    EXPECT_EQ(out[0].schedule.expire_ns, 1'000'000);
    EXPECT_EQ(out[0].schedule.node, &node);
}

TEST_F(TimerCommandQueueTest, DrainEmptyReturnsZero) {
    std::vector<TimerCommand> out;
    EXPECT_EQ(queue_.drain_all(out), 0u);
    EXPECT_TRUE(out.empty());
}

TEST_F(TimerCommandQueueTest, PushAndDrainMultiple) {
    for (size_t i = 0; i < 10; ++i) {
        auto cmd = TimerCommand::make_cancel(static_cast<uint64_t>(i));
        EXPECT_TRUE(queue_.try_push(cmd));
    }
    std::vector<TimerCommand> out;
    EXPECT_EQ(queue_.drain_all(out), 10u);
    for (size_t i = 0; i < 10; ++i) {
        EXPECT_EQ(out[i].type, TimerCommand::Type::Cancel);
        EXPECT_EQ(out[i].cancel.handle, static_cast<uint64_t>(i));
    }
}

TEST_F(TimerCommandQueueTest, DrainPreservesFifoOrder) {
    auto cmd1 = TimerCommand::make_cancel(100);
    auto cmd2 = TimerCommand::make_cancel(200);
    auto cmd3 = TimerCommand::make_cancel(300);
    queue_.try_push(cmd1);
    queue_.try_push(cmd2);
    queue_.try_push(cmd3);
    std::vector<TimerCommand> out;
    queue_.drain_all(out);
    ASSERT_EQ(out.size(), 3u);
    EXPECT_EQ(out[0].cancel.handle, 100u);
    EXPECT_EQ(out[1].cancel.handle, 200u);
    EXPECT_EQ(out[2].cancel.handle, 300u);
}

TEST_F(TimerCommandQueueTest, PushFullQueueReturnsFalse) {
    // Fill to usable capacity (kCapacity - 1; one slot reserved to
    // disambiguate full from empty).
    static constexpr size_t kUsable = TimerCommandQueue::kCapacity - 1;
    for (size_t i = 0; i < kUsable; ++i) {
        auto cmd = TimerCommand::make_cancel(i);
        EXPECT_TRUE(queue_.try_push(cmd)) << "push " << i << " should succeed";
    }
    // Next push must fail
    auto cmd = TimerCommand::make_cancel(999);
    EXPECT_FALSE(queue_.try_push(cmd)) << "push beyond capacity should fail";

    // Drain all
    std::vector<TimerCommand> out;
    queue_.drain_all(out);
    EXPECT_EQ(out.size(), kUsable);

    // Should succeed again
    EXPECT_TRUE(queue_.try_push(cmd));
}

class TimerHandleEncodingTest : public ::testing::Test {};

TEST_F(TimerHandleEncodingTest, EncodeDecodeRoundTrip) {
    auto h = TimerHandle::make_encoded(5, 12345, 7, 0);
    EXPECT_EQ(TimerHandle::shard_index(h), 5u);
    EXPECT_EQ(TimerHandle::slot_index(h), 12345u);
    EXPECT_EQ(TimerHandle::generation(h), 7u);
    EXPECT_EQ(TimerHandle::type_tag(h), 0u);
}

TEST_F(TimerHandleEncodingTest, MaxValuesPreserve) {
    auto h = TimerHandle::make_encoded(65535, 0xFFFFFF, 255, 65535);
    EXPECT_EQ(TimerHandle::shard_index(h), 65535u);
    EXPECT_EQ(TimerHandle::slot_index(h), 0xFFFFFFu);
    EXPECT_EQ(TimerHandle::generation(h), 255u);
    EXPECT_EQ(TimerHandle::type_tag(h), 65535u);
}

TEST_F(TimerHandleEncodingTest, DefaultHandleIsInvalid) {
    TimerHandle h;
    EXPECT_FALSE(h.valid());
}

TEST_F(TimerHandleEncodingTest, TypeTagZeroIsActorMessage) {
    auto h = TimerHandle::make_encoded(0, 100, 1, 0);
    EXPECT_EQ(TimerHandle::type_tag(h), 0u);
}

// ============================================================================
// TimerPlaneShard tests
// ============================================================================

class TimerPlaneShardTest : public ::testing::Test {
  protected:
    void SetUp() override {
        shard_ = new TimerPlaneShard(0, 1'000'000);
    }
    void TearDown() override {
        delete shard_;
    }
    TimerPlaneShard* shard_{nullptr};
};

TEST_F(TimerPlaneShardTest, ScheduleAndAdvanceFires) {
    bool fired = false;
    auto h = shard_->schedule(5'000'000, [&fired]() { fired = true; });
    EXPECT_TRUE(h.valid());
    int64_t now = 10'000'000;
    shard_->advance(now);
    EXPECT_TRUE(fired);
    EXPECT_EQ(shard_->pending_count(), 0u);
}

TEST_F(TimerPlaneShardTest, CancelPreventsFire) {
    bool fired = false;
    auto h = shard_->schedule(5'000'000, [&fired]() { fired = true; });
    EXPECT_TRUE(shard_->cancel(h));
    shard_->advance(10'000'000);
    EXPECT_FALSE(fired);
    EXPECT_EQ(shard_->cancelled_count(), 1u);
}

TEST_F(TimerPlaneShardTest, DoubleCancelReturnsFalse) {
    auto h = shard_->schedule(10'000'000, []() {});
    EXPECT_TRUE(shard_->cancel(h));
    EXPECT_FALSE(shard_->cancel(h)); // Already gone.
}

TEST_F(TimerPlaneShardTest, PendingCount) {
    shard_->schedule(5'000'000, []() {});
    shard_->schedule(10'000'000, []() {});
    EXPECT_EQ(shard_->pending_count(), 2u);
    shard_->advance(7'000'000);
    EXPECT_EQ(shard_->pending_count(), 1u);
    shard_->advance(12'000'000);
    EXPECT_EQ(shard_->pending_count(), 0u);
}

TEST_F(TimerPlaneShardTest, MinDeadline) {
    auto h1 = shard_->schedule(20'000'000, []() {}); // 20 ms
    auto h2 = shard_->schedule(5'000'000, []() {});  // 5 ms
    (void)h1;
    (void)h2;
    EXPECT_LT(shard_->min_deadline_ns(), 10'000'000LL);
}

} // namespace
} // namespace hpactor::sched
