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
