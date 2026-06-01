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

/// \brief Central trace manager: sampling, span lifecycle, ring buffer, and
///        drain thread.
///
/// Owns the sampler, exporter, id generator, and the MPSC ring buffer for
/// completed span records. A background drain thread periodically flushes
/// the ring buffer to the exporter.
///
/// \note Thread safety: start_span() and finish_span() are called from
///       actor scheduler threads. The drain thread runs independently.
///       The ring buffer is lock-free (MPSC). Exporter synchronization
///       is the exporter's responsibility.
class TraceManager {
  public:
    /// \brief Construct the trace manager.
    ///
    /// \param[in] config Trace subsystem configuration.
    /// \param[in] system Actor system pointer (may be nullptr for tests).
    /// \param[in] exporter Span exporter instance (nullptr = create from
    /// config).
    TraceManager(TraceConfig config, ActorSystem* system,
                 std::unique_ptr<SpanExporter> exporter = nullptr);

    /// \brief Stop the drain thread and shut down the exporter.
    ~TraceManager();

    TraceManager(const TraceManager&) = delete;
    TraceManager& operator=(const TraceManager&) = delete;

    /// \brief Start the background drain thread.
    void start();

    /// \brief Stop the background drain thread and flush remaining spans.
    void stop();

    /// \brief Force an immediate flush of the ring buffer.
    void force_flush();

    /// \brief Whether tracing is enabled.
    bool enabled() const noexcept {
        return config_.enabled;
    }

    /// \brief Read-only access to the trace configuration.
    const TraceConfig& config() const noexcept {
        return config_;
    }

    /// \brief Create a root trace context for an operation.
    ///
    /// Generates a new trace id and makes a sampling decision.
    ///
    /// \param[in] operation Operation name for the root span.
    /// \return A new root trace context.
    TraceContext create_root_context(std::string_view operation);

    /// \brief Create a child context from an existing parent.
    ///
    /// \param[in] parent The parent trace context.
    /// \return A child context with a new span id.
    TraceContext child_context(const TraceContext& parent);

    /// \brief Start a span and return a handle.
    ///
    /// The caller must call finish_span() when the operation completes.
    ///
    /// \param[in] start Span start parameters.
    /// \return A span handle (recording=false if not sampled).
    SpanHandle start_span(const SpanStart& start);

    /// \brief Complete a span and enqueue it for export.
    ///
    /// \param[in,out] span The span handle from start_span().
    /// \param[in] status Completion status.
    void finish_span(SpanHandle& span, SpanStatus status) noexcept;

    /// \brief Inject trace context into an outgoing message.
    ///
    /// Reads the current trace scope from the actor context and writes
    /// the W3C traceparent and tracestate headers into the message.
    ///
    /// \param[in,out] msg The message to inject context into.
    /// \param[in] ctx The sending actor's context.
    /// \param[in] allow_root Whether to create a root context if none exists.
    void inject_message_context(TypedMessage& msg, const ActorContext* ctx,
                                bool allow_root);

    /// \brief Number of spans dropped due to ring buffer overflow.
    ///
    /// \return Cumulative dropped span count.
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
