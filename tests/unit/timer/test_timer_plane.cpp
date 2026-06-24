// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

// tests/unit/timer/test_timer_plane.cpp

#include <hpactor/sched/scheduler_interfaces.hpp>
#include <hpactor/timer/timer_command_queue.hpp>
#include <hpactor/timer/timer_group.hpp>
#include <hpactor/timer/timer_plane.hpp>
#include <hpactor/timer/timer_plane_shard.hpp>

#include <atomic>
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

// ============================================================================
// TimerPlane tests
// ============================================================================

class TimerPlaneTest : public ::testing::Test {
  protected:
    void SetUp() override {
        plane_ = new TimerPlane(2, 1'000'000);
    }
    void TearDown() override {
        delete plane_;
    }
    TimerPlane* plane_{nullptr};
};

TEST_F(TimerPlaneTest, ScheduleFiresCallback) {
    bool fired = false;
    auto id = plane_->schedule(5'000'000, [&fired]() { fired = true; });
    EXPECT_NE(id, 0u);
    plane_->advance(10'000'000);
    EXPECT_TRUE(fired);
}

TEST_F(TimerPlaneTest, CancelPreventsCallback) {
    bool fired = false;
    auto id = plane_->schedule(5'000'000, [&fired]() { fired = true; });
    EXPECT_TRUE(plane_->cancel(id));
    plane_->advance(10'000'000);
    EXPECT_FALSE(fired);
}

TEST_F(TimerPlaneTest, NextDeadline) {
    plane_->schedule(20'000'000, []() {});
    plane_->schedule(5'000'000, []() {});
    int64_t dl = plane_->next_deadline();
    EXPECT_GT(dl, 0);
    EXPECT_LT(dl, 10'000'000LL); // ~5 ms from schedule time
}

TEST_F(TimerPlaneTest, Empty) {
    EXPECT_TRUE(plane_->empty());
    plane_->schedule(10'000'000, []() {});
    EXPECT_FALSE(plane_->empty());
}

TEST_F(TimerPlaneTest, Size) {
    EXPECT_EQ(plane_->size(), 0u);
    plane_->schedule(10'000'000, []() {});
    plane_->schedule(20'000'000, []() {});
    EXPECT_EQ(plane_->size(), 2u);
    plane_->advance(15'000'000);
    EXPECT_EQ(plane_->size(), 1u);
}

TEST_F(TimerPlaneTest, MultipleShardsFire) {
    std::atomic<int> count{0};
    auto cb = [&count]() { count.fetch_add(1); };
    plane_->schedule(3'000'000, cb);
    plane_->schedule(5'000'000, cb);
    plane_->schedule(7'000'000, cb);
    plane_->advance(10'000'000);
    EXPECT_EQ(count.load(), 3);
}

TEST_F(TimerPlaneTest, NumShards) {
    EXPECT_EQ(plane_->num_shards(), 2u);
}

TEST_F(TimerPlaneTest, CancelInvalidIdReturnsFalse) {
    EXPECT_FALSE(plane_->cancel(999999u));
}

// ============================================================================
// TimerGroup tests
// ============================================================================

class TimerGroupTest : public ::testing::Test {
  protected:
    void SetUp() override {
        plane_ = new TimerPlane(1, 1'000'000);
    }
    void TearDown() override {
        delete plane_;
    }
    TimerPlane* plane_{nullptr};
};

TEST_F(TimerGroupTest, AddAndRemoveHandle) {
    TimerGroup group;
    EXPECT_TRUE(group.empty());
    group.add(42);
    EXPECT_EQ(group.size(), 1u);
    group.add(99);
    EXPECT_EQ(group.size(), 2u);
    group.remove(42);
    EXPECT_EQ(group.size(), 1u);
}

TEST_F(TimerGroupTest, CancelAllInvokesCancelForAll) {
    TimerGroup group;

    // Schedule timers through the plane
    std::atomic<int> fired{0};
    auto h1 = plane_->schedule(100'000'000, [&fired]() { fired.fetch_add(1); });
    auto h2 = plane_->schedule(100'000'000, [&fired]() { fired.fetch_add(1); });
    auto h3 = plane_->schedule(100'000'000, [&fired]() { fired.fetch_add(1); });

    group.add(h1);
    group.add(h2);
    group.add(h3);
    EXPECT_EQ(group.size(), 3u);

    size_t cancelled = group.cancel_all(*plane_);
    EXPECT_EQ(cancelled, 3u);
    EXPECT_TRUE(group.empty());

    plane_->advance(200'000'000);
    EXPECT_EQ(fired.load(), 0);
}

TEST_F(TimerGroupTest, CancelAllOnEmptyGroupReturnsZero) {
    TimerGroup group;
    EXPECT_EQ(group.cancel_all(*plane_), 0u);
}

TEST_F(TimerGroupTest, RemoveNonexistentIsSafe) {
    TimerGroup group;
    group.add(100);
    group.remove(999); // no-op
    EXPECT_EQ(group.size(), 1u);
    group.remove(100);
    EXPECT_TRUE(group.empty());
}

} // namespace
} // namespace hpactor::sched
