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

#pragma once

#include <array>
#include <chrono>
#include <cstdint>

namespace hpactor {

/// \brief Sliding-window failure and timeout rate tracker.
///
/// Divides the observation window into \c kNumBuckets equal time
/// slices. Each bucket accumulates failure and timeout counts. Rates
/// are computed as events/sec summed across the non-expired window.
///
/// \note Thread safety: single-writer — only the owning actor's
///       scheduler thread calls these methods.
struct FailureRateTracker {
    /// Number of time-slice buckets in the sliding window.
    static constexpr size_t kNumBuckets = 10;

    /// Per-bucket failure counts. Index \c current_bucket is the
    /// active slice.
    std::array<uint32_t, kNumBuckets> failure_buckets{};
    /// Per-bucket timeout counts.
    std::array<uint32_t, kNumBuckets> timeout_buckets{};
    /// Index of the current (active) bucket.
    size_t current_bucket{0};
    /// Duration of each bucket in milliseconds. Derived from
    /// \c observation_window / \c kNumBuckets.
    uint32_t bucket_interval_ms{1000};
    /// Monotonic timestamp of the last bucket advancement.
    std::chrono::steady_clock::time_point last_bucket_advance{};

    /// \brief Record a processing failure in the current bucket.
    void record_failure() {
        failure_buckets[current_bucket]++;
    }

    /// \brief Record a timeout in the current bucket.
    void record_timeout() {
        timeout_buckets[current_bucket]++;
    }

    /// \brief Advance buckets if enough time has elapsed.
    ///
    /// Stale buckets are zeroed as the window slides forward. If the
    /// entire window has expired, all buckets are cleared.
    ///
    /// \param[in] now Current \c steady_clock time.
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

    /// \brief Failures per second over the observation window.
    ///
    /// \param[in] window_ms Total observation window in milliseconds.
    /// \return Sum of all bucket failure counts divided by the window
    ///         duration, in events/sec. Returns 0.0 if \p window_ms is 0.
    [[nodiscard]] double failure_rate(uint32_t window_ms) const {
        if (window_ms == 0)
            return 0.0;
        uint32_t total = 0;
        for (size_t i = 0; i < kNumBuckets; ++i) {
            total += failure_buckets[i];
        }
        return static_cast<double>(total) * 1000.0 / static_cast<double>(window_ms);
    }

    /// \brief Timeouts per second over the observation window.
    ///
    /// \param[in] window_ms Total observation window in milliseconds.
    /// \return Sum of all bucket timeout counts divided by the window
    ///         duration, in events/sec. Returns 0.0 if \p window_ms is 0.
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
