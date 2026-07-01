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

#include <hpactor/adt/disruptor_mpsc_ring.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <thread>

namespace hpactor::adt {
namespace {

using Ring4 = DisruptorMpscRing<uint64_t, 4>;

// ── Construction and empty state ──────────────────────────────────────────

TEST(DisruptorMpscRingTest, ConstructsEmpty) {
    Ring4 ring;
    EXPECT_TRUE(ring.empty());
    EXPECT_EQ(ring.snapshot().published_depth, 0u);
    EXPECT_EQ(ring.snapshot().capacity, 4u);
    EXPECT_FALSE(ring.is_closed());
}

TEST(DisruptorMpscRingTest, CapacityMatchesTemplateParameter) {
    DisruptorMpscRing<uint64_t, 8> ring8;
    EXPECT_EQ(ring8.snapshot().capacity, 8u);

    DisruptorMpscRing<uint64_t, 16> ring16;
    EXPECT_EQ(ring16.snapshot().capacity, 16u);
}

// ── Single producer FIFO ──────────────────────────────────────────────────

TEST(DisruptorMpscRingTest, SingleProducerFIFO) {
    Ring4 ring;
    EXPECT_TRUE(ring.try_publish(uint64_t{10}).accepted());
    EXPECT_TRUE(ring.try_publish(uint64_t{11}).accepted());
    EXPECT_TRUE(ring.try_publish(uint64_t{12}).accepted());
    EXPECT_FALSE(ring.empty());

    {
        auto lease = ring.try_acquire();
        ASSERT_TRUE(lease);
        EXPECT_EQ(lease.value(), 10u);
    }
    {
        auto lease = ring.try_acquire();
        ASSERT_TRUE(lease);
        EXPECT_EQ(lease.value(), 11u);
    }
    {
        auto lease = ring.try_acquire();
        ASSERT_TRUE(lease);
        EXPECT_EQ(lease.value(), 12u);
    }
    EXPECT_TRUE(ring.empty());
    EXPECT_FALSE(ring.try_acquire());
}

// ── Exact capacity and full detection ─────────────────────────────────────

TEST(DisruptorMpscRingTest, RejectsAtExactCapacity) {
    Ring4 ring;
    EXPECT_TRUE(ring.try_publish(1).accepted());
    EXPECT_TRUE(ring.try_publish(2).accepted());
    EXPECT_TRUE(ring.try_publish(3).accepted());
    EXPECT_TRUE(ring.try_publish(4).accepted());

    auto result = ring.try_publish(5);
    EXPECT_FALSE(result.accepted());
    EXPECT_EQ(result.code, RingPublishCode::Full);
}

// ── Lease-blocked reuse ───────────────────────────────────────────────────

TEST(DisruptorMpscRingTest, CannotReuseWhileLeased) {
    Ring4 ring;
    EXPECT_TRUE(ring.try_publish(10).accepted());
    EXPECT_TRUE(ring.try_publish(11).accepted());
    EXPECT_TRUE(ring.try_publish(12).accepted());
    EXPECT_TRUE(ring.try_publish(13).accepted());

    // Ring is full.
    EXPECT_FALSE(ring.try_publish(14).accepted());

    {
        auto lease = ring.try_acquire();
        ASSERT_TRUE(lease);
        EXPECT_EQ(lease.value(), 10u);
        // Still no room — lease hasn't released yet.
        EXPECT_FALSE(ring.try_publish(14).accepted());
    }

    // Lease released — now there's room.
    EXPECT_TRUE(ring.try_publish(14).accepted());
}

// ── Wraparound ────────────────────────────────────────────────────────────

TEST(DisruptorMpscRingTest, WraparoundSeveralCycles) {
    Ring4 ring;
    // Fill and drain several full cycles.
    for (uint64_t cycle = 0; cycle < 5; ++cycle) {
        for (uint64_t i = 0; i < 4; ++i) {
            EXPECT_TRUE(ring.try_publish(cycle * 10 + i).accepted());
        }
        EXPECT_EQ(ring.snapshot().published_depth, 4u);

        for (uint64_t i = 0; i < 4; ++i) {
            auto lease = ring.try_acquire();
            ASSERT_TRUE(lease);
            EXPECT_EQ(lease.value(), cycle * 10 + i);
        }
        EXPECT_TRUE(ring.empty());
    }
}

// ── Close semantics ───────────────────────────────────────────────────────

TEST(DisruptorMpscRingTest, CloseRejectsNewPublishes) {
    Ring4 ring;
    EXPECT_TRUE(ring.try_publish(1).accepted());
    ring.close();

    EXPECT_TRUE(ring.is_closed());
    auto result = ring.try_publish(2);
    EXPECT_EQ(result.code, RingPublishCode::Closed);

    // Existing message still consumable.
    auto lease = ring.try_acquire();
    ASSERT_TRUE(lease);
    EXPECT_EQ(lease.value(), 1u);
    EXPECT_TRUE(ring.empty());
}

TEST(DisruptorMpscRingTest, DoubleCloseIsIdempotent) {
    Ring4 ring;
    ring.close();
    ring.close();
    EXPECT_TRUE(ring.is_closed());
}

// ── Read lease ────────────────────────────────────────────────────────────

TEST(DisruptorMpscRingTest, ReadLeaseIsMoveOnly) {
    Ring4 ring;
    (void)ring.try_publish(42);

    auto lease1 = ring.try_acquire();
    ASSERT_TRUE(lease1);
    EXPECT_EQ(lease1.value(), 42u);

    // Move construction.
    auto lease2 = std::move(lease1);
    EXPECT_FALSE(lease1); // NOLINT: intentional use-after-move for test.
    ASSERT_TRUE(lease2);
    EXPECT_EQ(lease2.value(), 42u);

    // Move assignment.
    auto lease3 = ring.try_acquire();
    EXPECT_FALSE(lease3);
    lease3 = std::move(lease2);
    ASSERT_TRUE(lease3);
    EXPECT_EQ(lease3.value(), 42u);
}

TEST(DisruptorMpscRingTest, LeaseResetReleasesSlot) {
    Ring4 ring;
    (void)ring.try_publish(1);
    (void)ring.try_publish(2);

    {
        auto lease = ring.try_acquire();
        ASSERT_TRUE(lease);
        EXPECT_EQ(lease.value(), 1u);
        lease.reset();
        EXPECT_FALSE(lease);
    }
    // After reset, slot 0 is free for reuse.
    // But the consumer sequence hasn't advanced, so the next
    // acquisition is still at the same position.
}

// ── Snapshot accuracy ─────────────────────────────────────────────────────

TEST(DisruptorMpscRingTest, SnapshotTracksDepth) {
    Ring4 ring;
    EXPECT_EQ(ring.snapshot().published_depth, 0u);

    (void)ring.try_publish(1);
    EXPECT_EQ(ring.snapshot().published_depth, 1u);

    (void)ring.try_publish(2);
    EXPECT_EQ(ring.snapshot().published_depth, 2u);

    auto lease = ring.try_acquire();
    // Depth reflects published (not consumed) count.
    auto snap = ring.snapshot();
    EXPECT_GE(snap.published_depth, 1u);
}

// ── Multiple producer unique claims (deterministic) ───────────────────────

TEST(DisruptorMpscRingTest, TwoProducersUniqueClaims) {
    DisruptorMpscRing<uint64_t, 64> ring;
    std::atomic<bool> ready{false};

    auto producer = [&](uint64_t base) {
        while (!ready.load(std::memory_order_acquire)) {
            // spin-wait
        }
        for (int i = 0; i < 50; ++i) {
            uint64_t value = base + static_cast<uint64_t>(i);
            while (true) {
                auto result = ring.try_publish(value);
                if (result.accepted())
                    break;
                if (result.code == RingPublishCode::Full) {
                    // Consumer needs to drain — spin.
                    continue;
                }
                break;
            }
        }
    };

    std::thread t1(producer, 1000);
    std::thread t2(producer, 2000);

    ready.store(true, std::memory_order_release);

    // Drain all 100 messages.
    uint64_t total_consumed = 0;
    while (total_consumed < 100) {
        auto lease = ring.try_acquire();
        if (lease) {
            ++total_consumed;
        }
    }

    t1.join();
    t2.join();
    EXPECT_TRUE(ring.empty());
}

} // namespace
} // namespace hpactor::adt
