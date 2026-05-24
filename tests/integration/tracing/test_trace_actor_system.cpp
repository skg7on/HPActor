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

#include <hpactor/core/actor_system.hpp>

using namespace hpactor;

TEST(TraceActorSystemTest, TraceManagerNullWhenDisabled) {
    Config disabled;
    disabled.tracing.enabled = false;
    ActorSystem no_trace(disabled);
    EXPECT_EQ(no_trace.trace_manager(), nullptr);
}

TEST(TraceActorSystemTest, TraceManagerCreatedWhenEnabled) {
    Config enabled;
    enabled.tracing.enabled = true;
    enabled.tracing.exporter = tracing::TraceExporterKind::kMemory;
    enabled.tracing.sampler = tracing::SamplerKind::kAlwaysOn;
    ActorSystem with_trace(enabled);
    ASSERT_NE(with_trace.trace_manager(), nullptr);
    EXPECT_TRUE(with_trace.trace_manager()->enabled());
}