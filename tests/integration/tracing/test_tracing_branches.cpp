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

#include <cstring>
#include <fstream>
#include <memory>
#include <string>

#include <hpactor/actor/actor_context.hpp>
#include <hpactor/config/toml_parser.hpp>
#include <hpactor/tracing/memory_exporter.hpp>
#include <hpactor/tracing/sampler.hpp>
#include <hpactor/tracing/trace_config.hpp>
#include <hpactor/tracing/trace_context_parser.hpp>
#include <hpactor/tracing/trace_manager.hpp>

using namespace hpactor;
using namespace hpactor::tracing;
using namespace hpactor::config;

namespace {

std::string write_temp_toml(const std::string& content, const std::string& name) {
    std::string path = "/tmp/hpactor_test_" + name + ".toml";
    std::ofstream f(path);
    f << content;
    f.close();
    return path;
}

SpanStart make_span_start(const char* name, SpanKind kind = SpanKind::kInternal,
                          ActorId aid = ActorId{1}) {
    SpanStart s;
    s.name = name;
    s.kind = kind;
    s.actor_id = aid;
    s.has_parent = false;
    return s;
}

} // anonymous namespace

// ── 1. TraceContext W3C parsing and formatting ────────────────────────

TEST(TraceBranchesTest, TraceContextW3cParseAndFormat) {
    std::string traceparent =
        "00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-01";
    auto result = parse_w3c_trace_context(traceparent, "", 256);
    EXPECT_TRUE(result.ok());
    EXPECT_EQ(result.status(), TraceParseStatus::kOk);
    EXPECT_TRUE(result.context.valid());
    EXPECT_TRUE(result.context.sampled());

    std::string formatted = format_traceparent(result.context);
    EXPECT_EQ(formatted, traceparent);

    std::string not_sampled =
        "00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-00";
    auto result2 = parse_w3c_trace_context(not_sampled, "", 256);
    EXPECT_TRUE(result2.ok());
    EXPECT_FALSE(result2.context.sampled());

    std::string traceparent3 =
        "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01";
    std::string tracestate = "vendor1=opaqueValue";
    auto result3 = parse_w3c_trace_context(traceparent3, tracestate, 256);
    EXPECT_TRUE(result3.ok());
    EXPECT_EQ(result3.context.tracestate_view(), tracestate);
}

// ── 2. TraceContext W3C parse error paths ──────────────────────────────

TEST(TraceBranchesTest, TraceContextW3cParseErrors) {
    // Missing header
    {
        auto result = parse_w3c_trace_context("", "", 256);
        EXPECT_FALSE(result.ok());
        EXPECT_EQ(result.status(), TraceParseStatus::kMissing);
    }

    // Malformed traceparent
    {
        auto result = parse_w3c_trace_context("not-valid", "", 256);
        EXPECT_FALSE(result.ok());
    }

    // Invalid trace-id (all zeros)
    {
        std::string all_zero =
            "00-00000000000000000000000000000000-b7ad6b7169203331-01";
        auto result = parse_w3c_trace_context(all_zero, "", 256);
        EXPECT_FALSE(result.ok());
    }

    // Invalid span-id (all zeros)
    {
        std::string zero_span =
            "00-0af7651916cd43dd8448eb211c80319c-0000000000000000-01";
        auto result = parse_w3c_trace_context(zero_span, "", 256);
        EXPECT_FALSE(result.ok());
    }

    // Unsupported version
    {
        std::string bad_ver =
            "ff-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-01";
        auto result = parse_w3c_trace_context(bad_ver, "", 256);
        EXPECT_FALSE(result.ok());
        EXPECT_EQ(result.status(), TraceParseStatus::kUnsupportedVersion);
    }
}

// ── 3. Span lifecycle: start, finish, export ───────────────────────────

TEST(TraceBranchesTest, SpanLifecycleStartFinishExport) {
    TraceConfig cfg;
    cfg.enabled = true;
    cfg.exporter = TraceExporterKind::kMemory;
    cfg.sampler = SamplerKind::kAlwaysOn;
    cfg.export_interval = std::chrono::milliseconds(100);

    auto* mem = new MemoryExporter();
    TraceManager mgr(cfg, nullptr, std::unique_ptr<SpanExporter>(mem));
    mgr.start();

    SpanStart srv;
    srv.name = "http.request";
    srv.kind = SpanKind::kServer;
    srv.actor_id = ActorId{10};
    srv.has_parent = false;

    SpanHandle handle = mgr.start_span(srv);
    EXPECT_TRUE(handle.recording);
    EXPECT_TRUE(handle.context.valid());
    EXPECT_EQ(handle.kind, SpanKind::kServer);
    EXPECT_EQ(handle.actor_id, ActorId{10});

    mgr.finish_span(handle, SpanStatus::kOk);
    mgr.force_flush();

    auto spans = mem->snapshot();
    ASSERT_EQ(spans.size(), 1u);
    EXPECT_EQ(spans[0].kind, SpanKind::kServer);
    EXPECT_EQ(spans[0].status, SpanStatus::kOk);
    EXPECT_EQ(spans[0].actor_id, ActorId{10});

    mgr.stop();
}

// ── 4. TraceManager sampling decisions ─────────────────────────────────

TEST(TraceBranchesTest, TraceManagerSamplingDecisions) {
    // AlwaysOn
    {
        TraceConfig cfg;
        cfg.enabled = true;
        cfg.exporter = TraceExporterKind::kMemory;
        cfg.sampler = SamplerKind::kAlwaysOn;
        cfg.export_interval = std::chrono::milliseconds(100);

        auto* mem = new MemoryExporter();
        TraceManager mgr(cfg, nullptr, std::unique_ptr<SpanExporter>(mem));
        mgr.start();

        SpanStart s = make_span_start("always_on_span");
        SpanHandle h = mgr.start_span(s);
        EXPECT_TRUE(h.recording);
        mgr.finish_span(h, SpanStatus::kOk);
        mgr.force_flush();
        EXPECT_EQ(mem->snapshot().size(), 1u);
        mgr.stop();
    }

    // AlwaysOff
    {
        TraceConfig cfg;
        cfg.enabled = true;
        cfg.exporter = TraceExporterKind::kMemory;
        cfg.sampler = SamplerKind::kAlwaysOff;
        cfg.export_interval = std::chrono::milliseconds(100);

        auto* mem = new MemoryExporter();
        TraceManager mgr(cfg, nullptr, std::unique_ptr<SpanExporter>(mem));
        mgr.start();

        SpanStart s = make_span_start("always_off_span");
        SpanHandle h = mgr.start_span(s);
        EXPECT_FALSE(h.recording);
        mgr.finish_span(h, SpanStatus::kOk);
        mgr.force_flush();
        EXPECT_EQ(mem->snapshot().size(), 0u);
        mgr.stop();
    }
}

// ── 5. TraceManager parent-based sampling ──────────────────────────────

TEST(TraceBranchesTest, TraceManagerParentBasedSampling) {
    TraceConfig cfg;
    cfg.enabled = true;
    cfg.exporter = TraceExporterKind::kMemory;
    cfg.sampler = SamplerKind::kParentBasedTraceIdRatio;
    cfg.sample_ratio = 1.0;
    cfg.export_interval = std::chrono::milliseconds(100);

    auto* mem = new MemoryExporter();
    TraceManager mgr(cfg, nullptr, std::unique_ptr<SpanExporter>(mem));
    mgr.start();

    TraceContext root_ctx = mgr.create_root_context("root.operation");
    EXPECT_TRUE(root_ctx.valid());
    EXPECT_TRUE(root_ctx.sampled()) << "Root should be sampled with ratio=1.0";

    TraceContext child_ctx = mgr.child_context(root_ctx);
    EXPECT_TRUE(child_ctx.valid());
    EXPECT_EQ(child_ctx.trace_id, root_ctx.trace_id);
    EXPECT_NE(child_ctx.span_id, root_ctx.span_id);

    SpanStart child;
    child.name = "child.span";
    child.kind = SpanKind::kInternal;
    child.actor_id = ActorId{2};
    child.parent = root_ctx;
    child.has_parent = true;

    SpanHandle child_handle = mgr.start_span(child);
    EXPECT_TRUE(child_handle.recording);
    EXPECT_EQ(child_handle.context.trace_id, root_ctx.trace_id);
    EXPECT_NE(child_handle.context.span_id, root_ctx.span_id);

    mgr.finish_span(child_handle, SpanStatus::kOk);
    mgr.force_flush();

    auto spans = mem->snapshot();
    EXPECT_EQ(spans.size(), 1u);
    if (!spans.empty()) {
        EXPECT_EQ(spans[0].trace_id, root_ctx.trace_id);
    }

    mgr.stop();
}

// ── 6. TraceScope RAII (via ActorContext) ─────────────────────────────

TEST(TraceBranchesTest, TraceScopeRAIIViaActorContext) {
    ActorContext ctx(Actor{});

    EXPECT_FALSE(ctx.has_current_trace_context());

    TraceContext trace;
    trace.trace_id.bytes[0] = 0x0a;
    trace.trace_id.bytes[15] = 0x42;
    trace.span_id.bytes[0] = 0x0b;
    trace.span_id.bytes[7] = 0x43;

    {
        ActorContext::TraceScope scope(&ctx, trace);
        EXPECT_TRUE(ctx.has_current_trace_context());
        EXPECT_EQ(ctx.current_trace_context().trace_id, trace.trace_id);
        EXPECT_EQ(ctx.current_trace_context().span_id, trace.span_id);
    }

    EXPECT_FALSE(ctx.has_current_trace_context());
}

// ── 7. Nested TraceScope propagation ───────────────────────────────────

TEST(TraceBranchesTest, NestedTraceScopePropagation) {
    ActorContext ctx(Actor{});

    TraceContext outer;
    outer.trace_id.bytes[15] = 1;
    outer.span_id.bytes[7] = 0xAB;

    TraceContext inner;
    inner.trace_id.bytes[15] = 2;
    inner.span_id.bytes[7] = 0xCD;

    {
        ActorContext::TraceScope scope1(&ctx, outer);
        EXPECT_TRUE(ctx.has_current_trace_context());
        EXPECT_EQ(ctx.current_trace_context().trace_id.bytes[15], 1u);

        {
            ActorContext::TraceScope scope2(&ctx, inner);
            EXPECT_TRUE(ctx.has_current_trace_context());
            EXPECT_EQ(ctx.current_trace_context().trace_id.bytes[15], 2u);
        }

        EXPECT_TRUE(ctx.has_current_trace_context());
        EXPECT_EQ(ctx.current_trace_context().trace_id.bytes[15], 1u);
    }

    EXPECT_FALSE(ctx.has_current_trace_context());
}

// ── 8. Trace export to memory exporter ─────────────────────────────────

TEST(TraceBranchesTest, TraceExportToMemoryExporter) {
    MemoryExporter exporter;

    SpanRecord r1;
    r1.trace_id.bytes[15] = 1;
    r1.span_id.bytes[7] = 10;
    r1.actor_id = ActorId{100};
    r1.kind = SpanKind::kProducer;
    r1.status = SpanStatus::kOk;

    auto res = exporter.export_batch(std::span<const SpanRecord>(&r1, 1));
    EXPECT_TRUE(res.has_value());

    auto snapshot = exporter.snapshot();
    ASSERT_EQ(snapshot.size(), 1u);
    EXPECT_EQ(snapshot[0].actor_id, ActorId{100});
    EXPECT_EQ(snapshot[0].kind, SpanKind::kProducer);

    SpanRecord r2;
    r2.trace_id.bytes[15] = 2;
    r2.span_id.bytes[7] = 20;
    r2.actor_id = ActorId{200};
    r2.kind = SpanKind::kConsumer;
    r2.status = SpanStatus::kError;

    SpanRecord records[] = {r1, r2};
    res = exporter.export_batch(std::span<const SpanRecord>(records, 2));
    EXPECT_TRUE(res.has_value());

    auto snapshot2 = exporter.snapshot();
    EXPECT_EQ(snapshot2.size(), 3u);

    exporter.clear();
    auto snapshot3 = exporter.snapshot();
    EXPECT_EQ(snapshot3.size(), 0u);
}

// ── 9. Multiple spans correlation (parent/child) ───────────────────────

TEST(TraceBranchesTest, MultipleSpansCorrelation) {
    TraceConfig cfg;
    cfg.enabled = true;
    cfg.exporter = TraceExporterKind::kMemory;
    cfg.sampler = SamplerKind::kAlwaysOn;
    cfg.export_interval = std::chrono::milliseconds(100);

    auto* mem = new MemoryExporter();
    TraceManager mgr(cfg, nullptr, std::unique_ptr<SpanExporter>(mem));
    mgr.start();

    SpanStart root_start;
    root_start.name = "ingress.http";
    root_start.kind = SpanKind::kServer;
    root_start.actor_id = ActorId{1};
    root_start.has_parent = false;

    SpanHandle root = mgr.start_span(root_start);
    EXPECT_TRUE(root.recording);
    TraceId root_trace_id = root.context.trace_id;
    SpanId root_span_id = root.context.span_id;

    SpanStart db_start;
    db_start.name = "db.lookup";
    db_start.kind = SpanKind::kClient;
    db_start.actor_id = ActorId{2};
    db_start.parent = root.context;
    db_start.has_parent = true;

    SpanHandle db = mgr.start_span(db_start);
    EXPECT_TRUE(db.recording);
    EXPECT_EQ(db.context.trace_id, root_trace_id);
    EXPECT_NE(db.context.span_id, root_span_id);
    mgr.finish_span(db, SpanStatus::kOk);

    SpanStart cache_start;
    cache_start.name = "cache.get";
    cache_start.kind = SpanKind::kClient;
    cache_start.actor_id = ActorId{3};
    cache_start.parent = root.context;
    cache_start.has_parent = true;

    SpanHandle cache = mgr.start_span(cache_start);
    EXPECT_TRUE(cache.recording);
    EXPECT_EQ(cache.context.trace_id, root_trace_id);
    mgr.finish_span(cache, SpanStatus::kError);

    mgr.finish_span(root, SpanStatus::kOk);
    mgr.force_flush();

    auto spans = mem->snapshot();
    ASSERT_EQ(spans.size(), 3u);

    for (const auto& s : spans) {
        EXPECT_EQ(s.trace_id, root_trace_id);
    }

    int root_count = 0, db_count = 0, cache_count = 0;
    for (const auto& s : spans) {
        if (s.kind == SpanKind::kServer)
            root_count++;
        if (s.actor_id == ActorId{2})
            db_count++;
        if (s.actor_id == ActorId{3})
            cache_count++;
        if (s.status == SpanStatus::kError)
            EXPECT_EQ(s.actor_id, ActorId{3});
    }
    EXPECT_EQ(root_count, 1);
    EXPECT_EQ(db_count, 1);
    EXPECT_EQ(cache_count, 1);

    mgr.stop();
}

// ── 10. Tracing config parsing via TOML ─────────────────────────────────

TEST(TraceBranchesTest, TracingConfigTomlParsing) {
    std::string content = R"(
[system]
version = "1.0"

[system.tracing]
enabled = true
sampler = "trace_id_ratio"
sample_ratio = 0.5
exporter = "memory"
service_name = "test-service"
ring_buffer_capacity = 32768
export_interval_ms = 1000
max_export_batch_size = 256
record_actor_receive_spans = false
record_local_producer_spans = true
record_payload_size = false
propagate_unsampled = false

[[actor]]
id = "echo"
behavior = "EchoActor"
)";
    std::string path = write_temp_toml(content, "tracing_branches");
    auto result = TomlParser::parse(path);
    ASSERT_TRUE(result.has_value());

    const auto& tracing = result.value().system.tracing;
    EXPECT_TRUE(tracing.enabled);
    EXPECT_EQ(tracing.sampler, SamplerKind::kTraceIdRatio);
    EXPECT_DOUBLE_EQ(tracing.sample_ratio, 0.5);
    EXPECT_EQ(tracing.exporter, TraceExporterKind::kMemory);
    EXPECT_EQ(tracing.service_name, "test-service");
    EXPECT_EQ(tracing.ring_buffer_capacity, 32768u);
    EXPECT_EQ(tracing.export_interval, std::chrono::milliseconds(1000));
    EXPECT_EQ(tracing.max_export_batch_size, 256u);
    EXPECT_FALSE(tracing.record_actor_receive_spans);
    EXPECT_TRUE(tracing.record_local_producer_spans);
    EXPECT_FALSE(tracing.record_payload_size);
    EXPECT_FALSE(tracing.propagate_unsampled);
}

// ── 11. TraceConfig defaults ────────────────────────────────────────────

TEST(TraceBranchesTest, TraceConfigDefaults) {
    TraceConfig cfg;

    EXPECT_FALSE(cfg.enabled);
    EXPECT_TRUE(cfg.propagate_unsampled);
    EXPECT_EQ(cfg.ring_buffer_capacity, 65536u);
    EXPECT_EQ(cfg.service_name, "hpactor");
    EXPECT_EQ(cfg.sampler, SamplerKind::kParentBasedTraceIdRatio);
    EXPECT_DOUBLE_EQ(cfg.sample_ratio, 0.01);
    EXPECT_EQ(cfg.exporter, TraceExporterKind::kOtlpHttp);
    EXPECT_EQ(cfg.otlp_endpoint, "http://127.0.0.1:4318/v1/traces");
    EXPECT_TRUE(cfg.record_actor_receive_spans);
    EXPECT_TRUE(cfg.record_remote_producer_spans);
    EXPECT_FALSE(cfg.record_local_producer_spans);
    EXPECT_TRUE(cfg.record_payload_size);
    EXPECT_FALSE(cfg.create_roots_for_actor_context_sends);
    EXPECT_TRUE(cfg.create_roots_for_rpc);
    EXPECT_TRUE(cfg.create_roots_for_http_ingress);
}

// ── 12. Sampler variants ───────────────────────────────────────────────

TEST(TraceBranchesTest, SamplerVariants) {
    SamplingParameters params;
    params.trace_id.bytes[15] = 0xAB;
    params.has_parent = false;
    params.parent_sampled = false;

    // AlwaysOn
    {
        AlwaysOnSampler sampler;
        auto decision = sampler.should_sample(params);
        EXPECT_TRUE(decision.sampled);
    }

    // AlwaysOff
    {
        AlwaysOffSampler sampler;
        auto decision = sampler.should_sample(params);
        EXPECT_FALSE(decision.sampled);
    }

    // TraceIdRatio at 0.0 -> never sample
    {
        TraceIdRatioSampler sampler(0.0);
        auto decision = sampler.should_sample(params);
        EXPECT_FALSE(decision.sampled);
    }

    // TraceIdRatio at 1.0 -> always sample
    {
        TraceIdRatioSampler sampler(1.0);
        auto decision = sampler.should_sample(params);
        EXPECT_TRUE(decision.sampled);
    }

    // ParentBased: parent sampled -> child sampled
    {
        ParentBasedSampler sampler(0.0);
        SamplingParameters child_params;
        child_params.trace_id.bytes[15] = 0xCD;
        child_params.has_parent = true;
        child_params.parent_sampled = true;
        auto decision = sampler.should_sample(child_params);
        EXPECT_TRUE(decision.sampled);
    }

    // ParentBased: parent not sampled -> child not sampled
    {
        ParentBasedSampler sampler(1.0);
        SamplingParameters child_params;
        child_params.trace_id.bytes[15] = 0xEF;
        child_params.has_parent = true;
        child_params.parent_sampled = false;
        auto decision = sampler.should_sample(child_params);
        EXPECT_FALSE(decision.sampled);
    }

    // ParentBased: root span uses ratio
    {
        ParentBasedSampler sampler(1.0);
        SamplingParameters root_params;
        root_params.trace_id.bytes[15] = 0x11;
        root_params.has_parent = false;
        root_params.parent_sampled = false;
        auto decision = sampler.should_sample(root_params);
        EXPECT_TRUE(decision.sampled);
    }
}

// ── 13. TraceManager enabled/disabled ───────────────────────────────────

TEST(TraceBranchesTest, TraceManagerEnabledDisabledSwitch) {
    {
        TraceConfig cfg;
        cfg.enabled = false;
        TraceManager mgr(cfg, nullptr, nullptr);
        EXPECT_FALSE(mgr.enabled());
    }

    {
        TraceConfig cfg;
        cfg.enabled = true;
        cfg.exporter = TraceExporterKind::kMemory;
        TraceManager mgr(cfg, nullptr, nullptr);
        EXPECT_TRUE(mgr.enabled());
    }
}

// ── 14. TraceIdGenerator produces unique values ─────────────────────────

TEST(TraceBranchesTest, TraceIdGeneratorProducesUniqueIds) {
    TraceIdGenerator gen;

    TraceId t1 = gen.next_trace_id();
    TraceId t2 = gen.next_trace_id();
    TraceId t3 = gen.next_trace_id();

    EXPECT_TRUE(t1.valid());
    EXPECT_TRUE(t2.valid());
    EXPECT_TRUE(t3.valid());

    bool all_unique = !(t1 == t2) && !(t1 == t3) && !(t2 == t3);
    EXPECT_TRUE(all_unique);

    SpanId s1 = gen.next_span_id();
    SpanId s2 = gen.next_span_id();
    EXPECT_TRUE(s1.valid());
    EXPECT_TRUE(s2.valid());
    EXPECT_FALSE(s1 == s2);
}
