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

#include <hpactor/actor/typed_message.hpp>
#include <hpactor/actor_context.hpp>
#include <hpactor/tracing/json_exporter.hpp>
#include <hpactor/tracing/memory_exporter.hpp>
#include <hpactor/tracing/otlp_exporter.hpp>
#include <hpactor/tracing/trace_manager.hpp>

#include <chrono>

namespace hpactor::tracing {

TraceManager::TraceManager(TraceConfig config, ActorSystem* system,
                           std::unique_ptr<SpanExporter> exporter)
    : config_(std::move(config)), system_(system), sampler_(make_sampler()),
      exporter_(std::move(exporter)) {
    if (!exporter_) {
        switch (config_.exporter) {
            case TraceExporterKind::kNoop:
            case TraceExporterKind::kMemory:
                exporter_ = std::make_unique<MemoryExporter>();
                break;
            case TraceExporterKind::kJsonFile:
                exporter_ =
                    std::make_unique<JsonFileExporter>(config_.json_file_path);
                break;
            case TraceExporterKind::kOtlpHttp:
                exporter_ =
                    std::make_unique<OtlpHttpExporter>(config_.otlp_endpoint);
                break;
        }
    }
}

TraceManager::~TraceManager() {
    stop();
}

std::unique_ptr<Sampler> TraceManager::make_sampler() const {
    switch (config_.sampler) {
        case SamplerKind::kAlwaysOff:
            return std::make_unique<AlwaysOffSampler>();
        case SamplerKind::kAlwaysOn:
            return std::make_unique<AlwaysOnSampler>();
        case SamplerKind::kTraceIdRatio:
            return std::make_unique<TraceIdRatioSampler>(config_.sample_ratio);
        case SamplerKind::kParentBasedTraceIdRatio:
            return std::make_unique<ParentBasedSampler>(config_.sample_ratio);
    }
    return std::make_unique<AlwaysOffSampler>();
}

uint64_t TraceManager::now_ns() noexcept {
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

void TraceManager::start() {
    if (!config_.enabled || running_.exchange(true)) {
        return;
    }
    drain_thread_ = std::thread([this]() {
        while (running_.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(config_.export_interval);
            drain_once();
        }
    });
}

void TraceManager::stop() {
    if (!running_.exchange(false)) {
        return;
    }
    if (drain_thread_.joinable()) {
        drain_thread_.join();
    }
    drain_once();
    if (exporter_) {
        exporter_->shutdown();
    }
}

void TraceManager::force_flush() {
    drain_once();
}

TraceContext TraceManager::create_root_context(std::string_view /*operation*/) {
    TraceContext ctx;
    ctx.trace_id = ids_.next_trace_id();
    ctx.span_id = ids_.next_span_id();
    SamplingParameters params;
    params.trace_id = ctx.trace_id;
    auto decision = sampler_->should_sample(params);
    ctx.flags.set_sampled(decision.sampled);
    return ctx;
}

TraceContext TraceManager::child_context(const TraceContext& parent) {
    TraceContext child = parent;
    child.span_id = ids_.next_span_id();
    return child;
}

SpanHandle TraceManager::start_span(const SpanStart& start) {
    SpanHandle handle;
    if (!config_.enabled) {
        return handle;
    }

    TraceContext ctx;
    if (start.has_parent && start.parent.valid()) {
        ctx = child_context(start.parent);
        SamplingParameters params;
        params.trace_id = ctx.trace_id;
        params.has_parent = true;
        params.parent_sampled = start.parent.sampled();
        ctx.flags.set_sampled(sampler_->should_sample(params).sampled);
        handle.parent_span_id = start.parent.span_id;
    } else {
        ctx = create_root_context(start.name);
    }

    handle.context = ctx;
    handle.start_ns = now_ns();
    handle.kind = start.kind;
    handle.actor_id = start.actor_id;
    handle.sender_actor_id = start.sender_actor_id;
    handle.type_tag = start.type_tag;
    handle.message_id = start.message_id;
    handle.payload_size = start.payload_size;
    handle.recording = ctx.sampled();
    return handle;
}

void TraceManager::finish_span(SpanHandle& span, SpanStatus status) noexcept {
    if (!span.recording) {
        return;
    }
    SpanRecord record;
    record.trace_id = span.context.trace_id;
    record.span_id = span.context.span_id;
    record.parent_span_id = span.parent_span_id;
    record.actor_id = span.actor_id;
    record.sender_actor_id = span.sender_actor_id;
    record.type_tag = static_cast<uint32_t>(span.type_tag);
    record.message_id = span.message_id.value();
    record.start_ns = span.start_ns;
    record.end_ns = now_ns();
    record.payload_size = span.payload_size;
    record.kind = span.kind;
    record.status = status;
    if (!ring_.try_push(record)) {
        spans_dropped_.fetch_add(1, std::memory_order_relaxed);
    }
    span.recording = false;
}

void TraceManager::inject_message_context(TypedMessage& msg, const ActorContext* ctx,
                                          bool allow_root) {
    if (!config_.enabled || msg.has_trace_context()) {
        return;
    }
    if (ctx != nullptr && ctx->has_current_trace_context()) {
        msg.set_trace_context(ctx->current_trace_context());
        return;
    }
    if (allow_root) {
        msg.set_trace_context(create_root_context("hpactor.message"));
    }
}

void TraceManager::drain_once() {
    if (!exporter_) {
        return;
    }
    std::vector<SpanRecord> batch;
    batch.reserve(config_.max_export_batch_size);
    ring_.drain([&](const SpanRecord& record) {
        batch.push_back(record);
        if (batch.size() >= config_.max_export_batch_size) {
            (void)exporter_->export_batch(
                std::span<const SpanRecord>(batch.data(), batch.size()));
            batch.clear();
        }
    });
    if (!batch.empty()) {
        (void)exporter_->export_batch(
            std::span<const SpanRecord>(batch.data(), batch.size()));
    }
}

} // namespace hpactor::tracing