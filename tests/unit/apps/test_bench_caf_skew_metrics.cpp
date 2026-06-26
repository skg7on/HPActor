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

#include <apps/bench_caf/caf_bench_metrics.hpp>

#include <gtest/gtest.h>

namespace bench_caf = hpactor::apps::bench_caf;

TEST(SkewMetrics, ComputesReceiverSkew) {
    std::vector<uint64_t> counts = {100, 120, 95, 108};
    auto [min_v, max_v] = bench_caf::compute_receiver_skew(counts);
    EXPECT_EQ(min_v, 95u);
    EXPECT_EQ(max_v, 120u);
}

TEST(SkewMetrics, ComputesSenderSpread) {
    std::vector<double> throughputs = {1000.0, 1200.0, 950.0};
    auto [min_v, max_v] = bench_caf::compute_sender_spread(throughputs);
    EXPECT_DOUBLE_EQ(min_v, 950.0);
    EXPECT_DOUBLE_EQ(max_v, 1200.0);
}

TEST(SkewMetrics, BuildsSizeHistogram) {
    std::vector<size_t> sizes = {0, 16, 64, 256, 1024, 4096, 16384};
    auto hist = bench_caf::build_size_histogram(sizes);
    EXPECT_EQ(hist.size(), bench_caf::kHistogramBuckets);
    EXPECT_GT(hist[0], 0u);
    EXPECT_GT(hist[3], 0u);
    EXPECT_GT(hist[6], 0u);
}

TEST(SkewMetrics, EmptySkewReturnsZero) {
    std::vector<uint64_t> empty;
    auto [min_v, max_v] = bench_caf::compute_receiver_skew(empty);
    EXPECT_EQ(min_v, 0u);
    EXPECT_EQ(max_v, 0u);
}

TEST(SkewMetrics, EmptySpreadReturnsZero) {
    std::vector<double> empty;
    auto [min_v, max_v] = bench_caf::compute_sender_spread(empty);
    EXPECT_DOUBLE_EQ(min_v, 0.0);
    EXPECT_DOUBLE_EQ(max_v, 0.0);
}
