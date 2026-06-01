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

#include <hpactor/tracing/trace_exporter.hpp>

#include <string>

namespace hpactor::tracing {

/// \brief OTLP HTTP/protobuf span exporter.
///
/// Sends batches of SpanRecords to an OpenTelemetry collector via
/// HTTP POST with protobuf-encoded payloads.
///
/// \note Thread safety: export_batch() is called from the drain thread.
///       No internal locking — the drain thread is the sole caller.
class OtlpHttpExporter final : public SpanExporter {
  public:
    /// \brief Construct with an OTLP collector endpoint.
    ///
    /// \param[in] endpoint Full URL of the OTLP HTTP endpoint
    ///                    (e.g. http://127.0.0.1:4318/v1/traces).
    explicit OtlpHttpExporter(std::string endpoint);

    /// \brief Export a batch of spans via HTTP POST.
    ///
    /// \param[in] batch Span batch to export.
    /// \return success or an error on network failure.
    result<void> export_batch(std::span<const SpanRecord> batch) noexcept override;

    /// \brief No-op (no persistent resources to release).
    void shutdown() noexcept override {}

    /// \brief Exporter name.
    const char* name() const noexcept override {
        return "otlp_http";
    }

    /// \brief Build the JSON payload for a batch (for testing only).
    ///
    /// \param[in] batch Span batch.
    /// \param[in] service_name Service name for the resource span.
    /// \return The JSON string that would be sent to the collector.
    std::string build_json_payload_for_test(std::span<const SpanRecord> batch,
                                            const std::string& service_name) const;

  private:
    std::string endpoint_;
};

} // namespace hpactor::tracing
