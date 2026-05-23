#include <gtest/gtest.h>

#include <memory>

#include <hpactor/tracing/memory_exporter.hpp>
#include <hpactor/tracing/trace_manager.hpp>

using namespace hpactor;
using namespace hpactor::tracing;

TEST(TraceManagerTest, DisabledManagerDoesNotRecord) {
    TraceConfig disabled_cfg;
    disabled_cfg.enabled = false;
    auto disabled_exporter = std::make_unique<MemoryExporter>();
    TraceManager disabled(disabled_cfg, nullptr, std::move(disabled_exporter));
    SpanStart disabled_start;
    disabled_start.name = "disabled";
    SpanHandle disabled_span = disabled.start_span(disabled_start);
    EXPECT_FALSE(disabled_span.recording);
}

TEST(TraceManagerTest, EnabledManagerRecordsAndExportsSpans) {
    TraceConfig cfg;
    cfg.enabled = true;
    cfg.exporter = TraceExporterKind::kMemory;
    cfg.sampler = SamplerKind::kAlwaysOn;
    cfg.export_interval = std::chrono::milliseconds(300);
    auto* memory = new MemoryExporter();
    TraceManager manager(cfg, nullptr, std::unique_ptr<SpanExporter>(memory));
    manager.start();

    SpanStart start;
    start.name = "hpactor.test";
    start.kind = SpanKind::kInternal;
    start.actor_id = ActorId{42};
    SpanHandle span = manager.start_span(start);
    EXPECT_TRUE(span.recording);
    EXPECT_TRUE(span.context.valid());
    manager.finish_span(span, SpanStatus::kOk);
    manager.force_flush();

    auto spans = memory->snapshot();
    ASSERT_EQ(spans.size(), 1u);
    EXPECT_EQ(spans[0].actor_id, ActorId{42});
    EXPECT_EQ(spans[0].status, SpanStatus::kOk);
    manager.stop();
}
