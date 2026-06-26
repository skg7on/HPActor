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
#include <apps/bench_caf/caf_bench_sampler.hpp>

#include <gtest/gtest.h>

namespace bench_caf = hpactor::apps::bench_caf;

TEST(BenchCafMetrics, ComputesThroughput) {
    EXPECT_DOUBLE_EQ(bench_caf::throughput(2000, 1000), 2000.0);
    EXPECT_DOUBLE_EQ(bench_caf::throughput(5, 0), 0.0);
}

TEST(BenchCafMetrics, ComputesPeakRss) {
    EXPECT_EQ(bench_caf::peak_rss({7, 11, 3}), 11u);
    EXPECT_EQ(bench_caf::peak_rss({}), 0u);
}

TEST(BenchCafSampler, SnapshotIsNonNegative) {
    auto sample = bench_caf::sample_current_rss_bytes();
    EXPECT_GE(sample, 0u);
}
