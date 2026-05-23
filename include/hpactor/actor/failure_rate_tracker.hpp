// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <array>
#include <chrono>
#include <cstdint>

namespace hpactor {

/// Per-actor sliding-window failure and timeout rate tracker.
///
/// Divides the observation window into \c kNumBuckets time slices. Each
/// bucket accumulates failure and timeout counts. Rates are computed as
/// events/sec summed across the current window.
///
/// Single-writer: only the scheduler thread that owns the actor.
struct FailureRateTracker {
    static constexpr size_t kNumBuckets = 10;

    std::array<uint32_t, kNumBuckets> failure_buckets{};
    std::array<uint32_t, kNumBuckets> timeout_buckets{};
    size_t current_bucket{0};
    uint32_t bucket_interval_ms{1000}; // observation_window / kNumBuckets
    std::chrono::steady_clock::time_point last_bucket_advance{};

    /// Record a processing failure in the current bucket.
    void record_failure() {
        failure_buckets[current_bucket]++;
    }

    /// Record a timeout in the current bucket.
    void record_timeout() {
        timeout_buckets[current_bucket]++;
    }

    /// Advance buckets if enough time has elapsed. Old buckets are zeroed.
    /// \param now Current steady_clock time.
    void advance_buckets(std::chrono::steady_clock::time_point now) {
        if (bucket_interval_ms == 0)
            return;
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           now - last_bucket_advance)
                           .count();
        if (elapsed <= 0)
            return;
        size_t steps = static_cast<size_t>(elapsed) / bucket_interval_ms;
        if (steps == 0)
            return;
        if (steps >= kNumBuckets) {
            // Entire window expired — clear all.
            for (size_t i = 0; i < kNumBuckets; ++i) {
                failure_buckets[i] = 0;
                timeout_buckets[i] = 0;
            }
            current_bucket = 0;
        } else {
            for (size_t s = 0; s < steps; ++s) {
                current_bucket = (current_bucket + 1) % kNumBuckets;
                failure_buckets[current_bucket] = 0;
                timeout_buckets[current_bucket] = 0;
            }
        }
        last_bucket_advance = now;
    }

    /// Failures/sec over the observation window.
    /// \param window_ms Total observation window in milliseconds.
    [[nodiscard]] double failure_rate(uint32_t window_ms) const {
        if (window_ms == 0)
            return 0.0;
        uint32_t total = 0;
        for (size_t i = 0; i < kNumBuckets; ++i) {
            total += failure_buckets[i];
        }
        return static_cast<double>(total) * 1000.0 / static_cast<double>(window_ms);
    }

    /// Timeouts/sec over the observation window.
    /// \param window_ms Total observation window in milliseconds.
    [[nodiscard]] double timeout_rate(uint32_t window_ms) const {
        if (window_ms == 0)
            return 0.0;
        uint32_t total = 0;
        for (size_t i = 0; i < kNumBuckets; ++i) {
            total += timeout_buckets[i];
        }
        return static_cast<double>(total) * 1000.0 / static_cast<double>(window_ms);
    }
};

} // namespace hpactor
