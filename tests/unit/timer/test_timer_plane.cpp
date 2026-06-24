// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

// tests/unit/timer/test_timer_plane.cpp

#include <hpactor/timer/timer_command_queue.hpp>

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

} // namespace
} // namespace hpactor::sched
