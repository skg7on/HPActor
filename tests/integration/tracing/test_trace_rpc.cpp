#include <gtest/gtest.h>

#include <hpactor/rpc/rpc_channel.hpp>
#include <hpactor/tracing/memory_exporter.hpp>
#include <hpactor/tracing/trace_manager.hpp>

using namespace hpactor;

TEST(TraceRpcTest, PendingCallStoresTraceContext) {
    tracing::TraceConfig cfg;
    cfg.enabled = true;
    cfg.sampler = tracing::SamplerKind::kAlwaysOn;
    auto* memory = new tracing::MemoryExporter();
    tracing::TraceManager manager(cfg, nullptr,
                                  std::unique_ptr<tracing::SpanExporter>(memory));
    manager.start();

    TraceContext parent = manager.create_root_context("rpc-test");
    PendingCall call;
    call.msg_id = generate_message_id();
    call.target = ActorAddress{LocalEndpoint, ActorType{1}, ActorId{2}, 0};
    call.has_trace_context = true;
    call.trace_context = parent;
    EXPECT_TRUE(call.has_trace_context);
    EXPECT_EQ(call.trace_context.trace_id, parent.trace_id);
    manager.stop();
}
