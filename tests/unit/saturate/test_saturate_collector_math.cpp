// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

namespace hpactor::apps::bench_saturate {
namespace {

// =============================================================================
// Percentile computation (pure function, testable without ActorSystem)
// =============================================================================

struct PercentileResult {
    double p50 = 0.0;
    double p99 = 0.0;
    double p999 = 0.0;
};

PercentileResult compute_percentiles(const std::vector<double>& sorted) {
    PercentileResult r;
    if (sorted.empty())
        return r;
    r.p50 = sorted[sorted.size() / 2];
    r.p99 = sorted[sorted.size() * 99 / 100];
    r.p999 = sorted[sorted.size() * 999 / 1000];
    return r;
}

// =============================================================================
// Drop rate calculation
// =============================================================================

double compute_drop_rate_pct(uint64_t dropped, uint64_t total_sent) {
    if (total_sent == 0)
        return 0.0;
    return 100.0 * static_cast<double>(dropped) / static_cast<double>(total_sent);
}

// =============================================================================
// Throughput calculation
// =============================================================================

double compute_throughput_msgps(uint64_t total_msgs, uint64_t elapsed_ms) {
    if (elapsed_ms == 0)
        return 0.0;
    return static_cast<double>(total_msgs) /
           (static_cast<double>(elapsed_ms) / 1000.0);
}

// =============================================================================
// Tests: Percentiles
// =============================================================================

TEST(CollectorMathTest, PercentilesOddCount) {
    std::vector<double> data = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 100};
    auto r = compute_percentiles(data);
    EXPECT_DOUBLE_EQ(r.p50, 6.0);
    EXPECT_DOUBLE_EQ(r.p99, 100.0);
}

TEST(CollectorMathTest, PercentilesEvenCount) {
    std::vector<double> data = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    auto r = compute_percentiles(data);
    EXPECT_DOUBLE_EQ(r.p50, 6.0);
    EXPECT_DOUBLE_EQ(r.p99, 10.0);
}

TEST(CollectorMathTest, PercentilesSingleElement) {
    std::vector<double> data = {42.0};
    auto r = compute_percentiles(data);
    EXPECT_DOUBLE_EQ(r.p50, 42.0);
    EXPECT_DOUBLE_EQ(r.p99, 42.0);
    EXPECT_DOUBLE_EQ(r.p999, 42.0);
}

TEST(CollectorMathTest, PercentilesEmpty) {
    std::vector<double> data;
    auto r = compute_percentiles(data);
    EXPECT_DOUBLE_EQ(r.p50, 0.0);
    EXPECT_DOUBLE_EQ(r.p99, 0.0);
    EXPECT_DOUBLE_EQ(r.p999, 0.0);
}

TEST(CollectorMathTest, PercentilesAllSameValue) {
    std::vector<double> data(100, 5.0);
    auto r = compute_percentiles(data);
    EXPECT_DOUBLE_EQ(r.p50, 5.0);
    EXPECT_DOUBLE_EQ(r.p99, 5.0);
    EXPECT_DOUBLE_EQ(r.p999, 5.0);
}

// =============================================================================
// Tests: Drop rate
// =============================================================================

TEST(CollectorMathTest, DropRateZeroDrops) {
    EXPECT_DOUBLE_EQ(compute_drop_rate_pct(0, 1000000), 0.0);
}

TEST(CollectorMathTest, DropRateHalfDropped) {
    EXPECT_DOUBLE_EQ(compute_drop_rate_pct(500, 1000), 50.0);
}

TEST(CollectorMathTest, DropRateAllDropped) {
    EXPECT_DOUBLE_EQ(compute_drop_rate_pct(1000, 1000), 100.0);
}

TEST(CollectorMathTest, DropRateZeroTotal) {
    EXPECT_DOUBLE_EQ(compute_drop_rate_pct(0, 0), 0.0);
}

// =============================================================================
// Tests: Throughput
// =============================================================================

TEST(CollectorMathTest, ThroughputOneSecond) {
    EXPECT_DOUBLE_EQ(compute_throughput_msgps(1000, 1000), 1000.0);
}

TEST(CollectorMathTest, ThroughputHalfSecond) {
    EXPECT_DOUBLE_EQ(compute_throughput_msgps(500, 500), 1000.0);
}

TEST(CollectorMathTest, ThroughputZeroElapsed) {
    EXPECT_DOUBLE_EQ(compute_throughput_msgps(1000, 0), 0.0);
}

TEST(CollectorMathTest, ThroughputZeroMessages) {
    EXPECT_DOUBLE_EQ(compute_throughput_msgps(0, 1000), 0.0);
}

// =============================================================================
// Tests: Reservoir bound (streaming percentile helper)
// =============================================================================

TEST(CollectorMathTest, ReservoirBounded) {
    constexpr size_t kReservoirSize = 100;
    std::vector<double> reservoir;
    reservoir.reserve(kReservoirSize * 2);

    for (size_t i = 0; i < 500; ++i) {
        reservoir.push_back(static_cast<double>(i));
        if (reservoir.size() > kReservoirSize) {
            reservoir.erase(
                reservoir.begin(),
                reservoir.begin() +
                    static_cast<ptrdiff_t>(reservoir.size() - kReservoirSize));
        }
    }

    EXPECT_EQ(reservoir.size(), kReservoirSize);
    EXPECT_DOUBLE_EQ(reservoir.front(), 400.0);
    EXPECT_DOUBLE_EQ(reservoir.back(), 499.0);
}

// =============================================================================
// Tests: Ring buffer reservoir (lock-free replacement for vector
// erase-from-front)
// =============================================================================

// Simple ring buffer abstraction: fixed-size array + atomic write index.
// Used to validate the collector's ring buffer behavior in isolation.
class LatencyRingBuffer {
  public:
    static constexpr size_t kCapacity = 100;

    void store(double value) {
        size_t idx =
            write_count_.fetch_add(1, std::memory_order_relaxed) % kCapacity;
        slots_[idx].store(value, std::memory_order_relaxed);
    }

    void reset() {
        write_count_.store(0, std::memory_order_relaxed);
    }

    size_t count() const {
        size_t total = write_count_.load(std::memory_order_acquire);
        return total < kCapacity ? total : kCapacity;
    }

    std::vector<double> snapshot() const {
        size_t n = count();
        std::vector<double> out;
        out.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            out.push_back(slots_[i].load(std::memory_order_relaxed));
        }
        std::sort(out.begin(), out.end());
        return out;
    }

  private:
    std::array<std::atomic<double>, kCapacity> slots_{};
    std::atomic<size_t> write_count_{0};
};

TEST(CollectorRingBuffer, PartialFill) {
    LatencyRingBuffer rb;
    rb.store(10.0);
    rb.store(5.0);
    rb.store(20.0);
    EXPECT_EQ(rb.count(), 3u);
    auto snap = rb.snapshot();
    ASSERT_EQ(snap.size(), 3u);
    EXPECT_DOUBLE_EQ(snap[0], 5.0);
    EXPECT_DOUBLE_EQ(snap[1], 10.0);
    EXPECT_DOUBLE_EQ(snap[2], 20.0);
}

TEST(CollectorRingBuffer, FullCapacity) {
    LatencyRingBuffer rb;
    for (size_t i = 0; i < 100; ++i) {
        rb.store(static_cast<double>(100 - i)); // descending
    }
    EXPECT_EQ(rb.count(), 100u);
    auto snap = rb.snapshot();
    ASSERT_EQ(snap.size(), 100u);
    EXPECT_DOUBLE_EQ(snap[0], 1.0);    // min after sort
    EXPECT_DOUBLE_EQ(snap[99], 100.0); // max after sort
}

TEST(CollectorRingBuffer, WrapAroundOverwritesOldest) {
    LatencyRingBuffer rb;
    // Fill to capacity
    for (size_t i = 0; i < 100; ++i) {
        rb.store(static_cast<double>(i));
    }
    // Write 50 more — should wrap and overwrite oldest
    for (size_t i = 0; i < 50; ++i) {
        rb.store(static_cast<double>(1000 + i));
    }
    EXPECT_EQ(rb.count(), 100u);
    auto snap = rb.snapshot();
    ASSERT_EQ(snap.size(), 100u);
    // The 50 oldest (0-49) were overwritten by 1000-1049;
    // the 50 newest (50-99) remain.
    EXPECT_DOUBLE_EQ(snap[0], 50.0);    // oldest surviving
    EXPECT_DOUBLE_EQ(snap[99], 1049.0); // newest
}

TEST(CollectorRingBuffer, ResetClearsData) {
    LatencyRingBuffer rb;
    for (size_t i = 0; i < 50; ++i) {
        rb.store(static_cast<double>(i));
    }
    EXPECT_EQ(rb.count(), 50u);
    rb.reset();
    EXPECT_EQ(rb.count(), 0u);
    auto snap = rb.snapshot();
    EXPECT_TRUE(snap.empty());
}

TEST(CollectorRingBuffer, PercentileOnRingBufferData) {
    LatencyRingBuffer rb;
    // 1..100 sorted ascending
    for (size_t i = 0; i < 100; ++i) {
        rb.store(static_cast<double>(i + 1));
    }
    auto snap = rb.snapshot();
    ASSERT_EQ(snap.size(), 100u);
    // P50 = median = 51st element (index 50)
    EXPECT_DOUBLE_EQ(snap[50], 51.0);
    // P99 = index 99
    EXPECT_DOUBLE_EQ(snap[99], 100.0);
    // P999 = index 99 (only 100 elements)
    EXPECT_DOUBLE_EQ(snap[99], 100.0);
}

} // namespace
} // namespace hpactor::apps::bench_saturate
