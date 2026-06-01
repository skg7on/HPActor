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

#include <hpactor/tracing/span.hpp>
#include <hpactor/types/types.hpp>

#include <span>

namespace hpactor::tracing {

/// \brief Abstract interface for exporting completed span records.
///
/// Implementations include MemoryExporter (testing), JsonFileExporter
/// (NDJSON files), and OtlpHttpExporter (OTLP/HTTP to a collector).
///
/// \note Thread safety: export_batch() is called from the drain thread.
///       Implementations must be internally synchronized if they share
///       state with other threads (e.g. MemoryExporter::snapshot()).
class SpanExporter {
  public:
    virtual ~SpanExporter() = default;

    /// \brief Export a batch of completed span records.
    ///
    /// \param[in] batch Non-owning span of SpanRecord to export.
    /// \return success or an error code on export failure.
    virtual result<void>
    export_batch(std::span<const SpanRecord> batch) noexcept = 0;

    /// \brief Graceful shutdown. Called before the exporter is destroyed.
    virtual void shutdown() noexcept = 0;

    /// \brief Human-readable exporter name for diagnostics.
    ///
    /// \return A static string literal.
    virtual const char* name() const noexcept = 0;
};

} // namespace hpactor::tracing
