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
#include <hpactor/tracing/sampler.hpp>

using namespace hpactor;
using namespace hpactor::tracing;

static TraceId make_trace(uint8_t last) {
    TraceId id;
    id.bytes[15] = last;
    return id;
}

TEST(SamplerTest, AlwaysOn) {
    SamplingParameters params;
    params.trace_id = make_trace(1);

    AlwaysOnSampler on;
    EXPECT_TRUE(on.should_sample(params).sampled);
}

TEST(SamplerTest, AlwaysOff) {
    SamplingParameters params;
    params.trace_id = make_trace(1);

    AlwaysOffSampler off;
    EXPECT_FALSE(off.should_sample(params).sampled);
}

TEST(SamplerTest, TraceIdRatioZero) {
    SamplingParameters params;
    params.trace_id = make_trace(1);

    TraceIdRatioSampler zero(0.0);
    EXPECT_FALSE(zero.should_sample(params).sampled);
}

TEST(SamplerTest, TraceIdRatioOne) {
    SamplingParameters params;
    params.trace_id = make_trace(1);

    TraceIdRatioSampler one(1.0);
    EXPECT_TRUE(one.should_sample(params).sampled);
}

TEST(SamplerTest, ParentBasedSamplerOn) {
    SamplingParameters params;
    params.trace_id = make_trace(1);
    params.has_parent = true;
    params.parent_sampled = true;

    ParentBasedSampler parent_on(0.0);
    EXPECT_TRUE(parent_on.should_sample(params).sampled);
}

TEST(SamplerTest, ParentBasedSamplerOff) {
    SamplingParameters params;
    params.trace_id = make_trace(1);
    params.has_parent = true;
    params.parent_sampled = false;

    ParentBasedSampler parent_on(0.0);
    EXPECT_FALSE(parent_on.should_sample(params).sampled);
}
