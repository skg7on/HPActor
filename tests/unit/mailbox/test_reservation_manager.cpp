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

#include <hpactor/mailbox/detail/reservation_manager.hpp>

#include <gtest/gtest.h>
#include <thread>
#include <vector>

using namespace hpactor::mailbox::detail;

// Dummy type for template instantiation.
struct TestMsg {
    int payload = 0;
};

class ReservationManagerTest : public ::testing::Test {
  protected:
    ReservationManager<TestMsg> mgr;
};

TEST_F(ReservationManagerTest, ReserveWithinCountLimit) {
    auto r = mgr.try_reserve(100, 10, 0);
    EXPECT_EQ(r, ReservationResult::Reserved);
    EXPECT_EQ(mgr.reserved_count(), 1);
    EXPECT_EQ(mgr.queued_bytes(), 100);
}

TEST_F(ReservationManagerTest, RejectAtCountCapacity) {
    // Fill to capacity.
    for (int i = 0; i < 5; i++)
        EXPECT_EQ(mgr.try_reserve(1, 5, 0), ReservationResult::Reserved);
    auto r = mgr.try_reserve(1, 5, 0);
    EXPECT_EQ(r, ReservationResult::CountCapacity);
}

TEST_F(ReservationManagerTest, RejectAtByteCapacity) {
    auto r = mgr.try_reserve(200, 0, 100);
    EXPECT_EQ(r, ReservationResult::ByteCapacity);
}

TEST_F(ReservationManagerTest, TwoPhaseRollbackOnByteFailure) {
    // Reserve count slot, then fail on bytes — count should roll back.
    auto r = mgr.try_reserve(200, 1, 100);
    EXPECT_EQ(r, ReservationResult::ByteCapacity);
    // Count slot was released during rollback — another reserve should succeed.
    EXPECT_EQ(mgr.reserved_count(), 0);
    r = mgr.try_reserve(50, 1, 100);
    EXPECT_EQ(r, ReservationResult::Reserved);
}

TEST_F(ReservationManagerTest, ReleaseReturnsCapacity) {
    mgr.try_reserve(50, 1, 100);
    mgr.release(50);
    EXPECT_EQ(mgr.reserved_count(), 0);
    EXPECT_EQ(mgr.queued_bytes(), 0);
    // Should be able to reserve again.
    auto r = mgr.try_reserve(60, 1, 100);
    EXPECT_EQ(r, ReservationResult::Reserved);
}

TEST_F(ReservationManagerTest, SystemReserveBypassesByteBudget) {
    // Main pool exhausted at byte level.
    mgr.try_reserve(90, 5, 100);
    auto r = mgr.try_reserve(20, 5, 100);
    EXPECT_EQ(r, ReservationResult::ByteCapacity);
    // System reserve should still work.
    bool ok = mgr.try_reserve_system(200, 32);
    EXPECT_TRUE(ok);
    EXPECT_EQ(mgr.reserved_system_count(), 1);
}

TEST_F(ReservationManagerTest, SystemReserveRespectsLimit) {
    for (int i = 0; i < 3; i++)
        EXPECT_TRUE(mgr.try_reserve_system(1, 3));
    EXPECT_FALSE(mgr.try_reserve_system(1, 3));
}

TEST_F(ReservationManagerTest, ReleaseSystemReturnsCapacity) {
    mgr.try_reserve_system(10, 32);
    mgr.release_system(10);
    EXPECT_EQ(mgr.reserved_system_count(), 0);
}

TEST_F(ReservationManagerTest, UnlimitedMessagesCountsBytes) {
    // max_messages=0 means unlimited count.
    auto r = mgr.try_reserve(100, 0, 0);
    EXPECT_EQ(r, ReservationResult::Reserved);
    EXPECT_EQ(mgr.queued_bytes(), 100);
}

TEST_F(ReservationManagerTest, ConcurrentReservationsDontExceedCapacity) {
    constexpr int kThreads = 4;
    constexpr int kPerThread = 250;
    constexpr uint32_t kCap = kThreads * kPerThread;
    std::atomic<int> success{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; t++) {
        threads.emplace_back([&]() {
            for (int i = 0; i < kPerThread; i++) {
                if (mgr.try_reserve(1, kCap, 0) == ReservationResult::Reserved)
                    success.fetch_add(1);
            }
        });
    }
    for (auto& th : threads)
        th.join();
    EXPECT_EQ(success.load(), kCap);
    EXPECT_EQ(mgr.reserved_count(), kCap);
}
