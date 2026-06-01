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

#include <chrono>
#include <cstdint>
#include <string>

namespace hpactor::tracing {

/// \brief Span exporter backend selection.
enum class TraceExporterKind : uint8_t {
    kNoop,     ///< Discard all spans.
    kMemory,   ///< Accumulate spans in memory (for testing).
    kJsonFile, ///< Write spans as newline-delimited JSON.
    kOtlpHttp, ///< Export spans via OTLP HTTP/protobuf.
};

/// \brief Sampling strategy selection.
enum class SamplerKind : uint8_t {
    kAlwaysOff,               ///< Never sample.
    kAlwaysOn,                ///< Always sample.
    kTraceIdRatio,            ///< Sample based on trace id ratio.
    kParentBasedTraceIdRatio, ///< Respect parent decision; root spans use
                              ///< ratio-based sampling.
};

/// \brief Configuration for the distributed tracing subsystem.
///
/// Maps to TOML keys under \c [system.tracing].
struct TraceConfig {
    /// \brief Enable distributed tracing. Default false.
    bool enabled{false};
    /// \brief Propagate trace context even when not sampled.
    bool propagate_unsampled{true};
    /// \brief Capacity of the span ring buffer (must be a power of two).
    uint32_t ring_buffer_capacity{65536};
    /// \brief Service name included in exported spans.
    std::string service_name{"hpactor"};
    /// \brief Sampling strategy.
    SamplerKind sampler{SamplerKind::kParentBasedTraceIdRatio};
    /// \brief Sampling ratio (0.0–1.0) for ratio-based samplers.
    double sample_ratio{0.01};
    /// \brief Span exporter backend.
    TraceExporterKind exporter{TraceExporterKind::kOtlpHttp};
    /// \brief OTLP HTTP endpoint URL.
    std::string otlp_endpoint{"http://127.0.0.1:4318/v1/traces"};
    /// \brief Path for JSON file exporter output.
    std::string json_file_path;
    /// \brief Interval between drain/export cycles.
    std::chrono::milliseconds export_interval{500};
    /// \brief Maximum spans per export batch.
    uint32_t max_export_batch_size{512};
    /// \brief Maximum tracestate header length in bytes.
    uint16_t max_tracestate_len{256};
    /// \brief Record spans for actor message receive (consumer).
    bool record_actor_receive_spans{true};
    /// \brief Record spans for remote message sends (producer).
    bool record_remote_producer_spans{true};
    /// \brief Record spans for local message sends (producer).
    bool record_local_producer_spans{false};
    /// \brief Include payload size in span records.
    bool record_payload_size{true};
    /// \brief Create root spans for actor-context send operations.
    bool create_roots_for_actor_context_sends{false};
    /// \brief Create root spans for RPC calls.
    bool create_roots_for_rpc{true};
    /// \brief Create root spans for HTTP ingress.
    bool create_roots_for_http_ingress{true};
};

} // namespace hpactor::tracing
