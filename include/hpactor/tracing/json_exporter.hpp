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

#include <fstream>
#include <mutex>
#include <string>

namespace hpactor::tracing {

/// \brief Newline-delimited JSON (NDJSON) file exporter.
///
/// Writes each batch of spans as one JSON object per line. Suitable for
/// offline analysis or feeding into log-based trace processors.
///
/// \note Thread safety: internally synchronized via std::mutex on the
///       output file stream.
class JsonFileExporter final : public SpanExporter {
  public:
    /// \brief Construct with an output file path.
    ///
    /// \param[in] path Path to the output NDJSON file.
    explicit JsonFileExporter(std::string path);

    /// \brief Write a batch as NDJSON lines.
    ///
    /// \param[in] batch Span batch to write.
    /// \return success or an error on file write failure.
    result<void> export_batch(std::span<const SpanRecord> batch) noexcept override;

    /// \brief Close the output file.
    void shutdown() noexcept override;

    /// \brief Exporter name.
    const char* name() const noexcept override {
        return "json_file";
    }

  private:
    std::string path_;
    std::ofstream out_;
    std::mutex mutex_;
};

/// \brief Serialize a single SpanRecord to a JSON string.
///
/// \param[in] record The span record.
/// \return A JSON object string representing the span.
std::string span_record_to_json(const SpanRecord& record);

} // namespace hpactor::tracing
