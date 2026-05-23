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

#include <gtest/gtest.h>
#include <hpactor/actor/failure_rate_tracker.hpp>

#include <chrono>

using namespace hpactor;
using namespace std::chrono;
using Clock = steady_clock;

TEST(FailureRateTrackerTest, DefaultsAreZero) {
    FailureRateTracker t;
    EXPECT_EQ(t.current_bucket, 0u);
    EXPECT_EQ(t.failure_rate(10000), 0.0);
    EXPECT_EQ(t.timeout_rate(10000), 0.0);
}

TEST(FailureRateTrackerTest, RecordSingleFailure) {
    FailureRateTracker t;
    t.record_failure();
    EXPECT_EQ(t.failure_buckets[0], 1u);
}

TEST(FailureRateTrackerTest, RecordSingleTimeout) {
    FailureRateTracker t;
    t.record_timeout();
    EXPECT_EQ(t.timeout_buckets[0], 1u);
}

TEST(FailureRateTrackerTest, FailureRateComputed) {
    FailureRateTracker t;
    t.failure_buckets[0] = 5;
    t.failure_buckets[1] = 5;
    // 10 failures over a 10-bucket window of 10000ms.
    double rate = t.failure_rate(10000);
    EXPECT_DOUBLE_EQ(rate, 1.0); // 10 failures / 10s = 1.0/sec
}

TEST(FailureRateTrackerTest, TimeoutRateComputed) {
    FailureRateTracker t;
    t.timeout_buckets[0] = 20;
    // 20 timeouts over 10000ms.
    double rate = t.timeout_rate(10000);
    EXPECT_DOUBLE_EQ(rate, 2.0); // 20 timeouts / 10s = 2.0/sec
}

TEST(FailureRateTrackerTest, ZeroWindowReturnsZero) {
    FailureRateTracker t;
    t.failure_buckets[0] = 100;
    EXPECT_DOUBLE_EQ(t.failure_rate(0), 0.0);
    EXPECT_DOUBLE_EQ(t.timeout_rate(0), 0.0);
}

TEST(FailureRateTrackerTest, AdvanceBucketsClearsCurrent) {
    FailureRateTracker t;
    t.bucket_interval_ms = 1000;
    t.failure_buckets[0] = 5;
    t.last_bucket_advance = Clock::now() - milliseconds(1100);

    t.advance_buckets(Clock::now());

    // Moved to bucket 1 and cleared it.
    EXPECT_EQ(t.current_bucket, 1u);
    EXPECT_EQ(t.failure_buckets[1], 0u);
    // Bucket 0 preserved (still in window).
    EXPECT_EQ(t.failure_buckets[0], 5u);
}

TEST(FailureRateTrackerTest, AdvanceBucketsZeroInterval) {
    FailureRateTracker t;
    t.bucket_interval_ms = 0;
    t.failure_buckets[0] = 5;
    t.last_bucket_advance = Clock::now() - milliseconds(5000);

    t.advance_buckets(Clock::now());
    // No advance when interval is 0.
    EXPECT_EQ(t.current_bucket, 0u);
}

TEST(FailureRateTrackerTest, AdvanceBucketsFullWrapClearsAll) {
    FailureRateTracker t;
    t.bucket_interval_ms = 1000;
    for (size_t i = 0; i < FailureRateTracker::kNumBuckets; ++i) {
        t.failure_buckets[i] = 1;
    }
    t.last_bucket_advance = Clock::now() - milliseconds(15000);

    t.advance_buckets(Clock::now());

    // All buckets should be cleared (entire window expired).
    for (size_t i = 0; i < FailureRateTracker::kNumBuckets; ++i) {
        EXPECT_EQ(t.failure_buckets[i], 0u) << "bucket " << i;
    }
    EXPECT_EQ(t.current_bucket, 0u);
}
