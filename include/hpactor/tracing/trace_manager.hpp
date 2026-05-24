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

#pragma once

#include <hpactor/metrics/metrics_ring_buffer.hpp>
#include <hpactor/tracing/sampler.hpp>
#include <hpactor/tracing/span.hpp>
#include <hpactor/tracing/trace_config.hpp>
#include <hpactor/tracing/trace_exporter.hpp>

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

namespace hpactor {
class ActorContext;
class ActorSystem;
class TypedMessage;
} // namespace hpactor

namespace hpactor::tracing {

class TraceManager {
  public:
    TraceManager(TraceConfig config, ActorSystem* system,
                 std::unique_ptr<SpanExporter> exporter = nullptr);
    ~TraceManager();

    TraceManager(const TraceManager&) = delete;
    TraceManager& operator=(const TraceManager&) = delete;

    void start();
    void stop();
    void force_flush();

    bool enabled() const noexcept {
        return config_.enabled;
    }
    const TraceConfig& config() const noexcept {
        return config_;
    }

    TraceContext create_root_context(std::string_view operation);
    TraceContext child_context(const TraceContext& parent);

    SpanHandle start_span(const SpanStart& start);
    void finish_span(SpanHandle& span, SpanStatus status) noexcept;

    void inject_message_context(TypedMessage& msg, const ActorContext* ctx,
                                bool allow_root);

    uint64_t spans_dropped() const noexcept {
        return spans_dropped_.load(std::memory_order_relaxed);
    }

  private:
    std::unique_ptr<Sampler> make_sampler() const;
    void drain_once();
    static uint64_t now_ns() noexcept;

    TraceConfig config_;
    [[maybe_unused]] ActorSystem* system_{nullptr};
    TraceIdGenerator ids_;
    std::unique_ptr<Sampler> sampler_;
    std::unique_ptr<SpanExporter> exporter_;
    metrics::MpscRingBuffer<SpanRecord> ring_;
    std::atomic<bool> running_{false};
    std::thread drain_thread_;
    std::atomic<uint64_t> spans_dropped_{0};
};

} // namespace hpactor::tracing