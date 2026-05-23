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
#include <hpactor/tracing/trace_config.hpp>

TEST(TraceConfigTest, Defaults) {
    hpactor::tracing::TraceConfig cfg;
    EXPECT_FALSE(cfg.enabled);
    EXPECT_TRUE(cfg.propagate_unsampled);
    EXPECT_EQ(cfg.ring_buffer_capacity, 65536u);
    EXPECT_EQ(cfg.sampler, hpactor::tracing::SamplerKind::kParentBasedTraceIdRatio);
    EXPECT_EQ(cfg.exporter, hpactor::tracing::TraceExporterKind::kOtlpHttp);
    EXPECT_EQ(cfg.otlp_endpoint, "http://127.0.0.1:4318/v1/traces");
}
