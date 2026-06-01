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

#include <hpactor/types/types.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace hpactor::tracing {

/// \brief Result status for W3C trace context parsing.
enum class TraceParseStatus : uint8_t {
    kOk,                 ///< Successfully parsed.
    kMissing,            ///< No traceparent header present.
    kMalformed,          ///< Header format is invalid.
    kUnsupportedVersion, ///< traceparent version is not 00.
    kInvalidTraceId,     ///< Trace id is all-zero or malformed.
    kInvalidSpanId,      ///< Span id is all-zero or malformed.
    kTracestateTooLarge, ///< tracestate header exceeds the max length.
};

/// \brief Result of parsing W3C trace context headers.
struct TraceParseResult {
    /// \brief Parse status code.
    TraceParseStatus status_value{TraceParseStatus::kMissing};
    /// \brief Parsed trace context (valid only when ok() returns true).
    TraceContext context{};

    /// \brief Get the parse status.
    ///
    /// \return The status code.
    [[nodiscard]] TraceParseStatus status() const noexcept {
        return status_value;
    }

    /// \brief Whether parsing succeeded.
    ///
    /// \retval true Status is kOk.
    /// \retval false Parsing failed or the header was missing.
    [[nodiscard]] bool ok() const noexcept {
        return status_value == TraceParseStatus::kOk;
    }
};

/// \brief Parse W3C traceparent and tracestate headers into a TraceContext.
///
/// \param[in] traceparent The \c traceparent header value.
/// \param[in] tracestate The \c tracestate header value.
/// \param[in] max_tracestate_len Maximum allowed tracestate length in bytes.
/// \return A TraceParseResult with the parsed context and status.
TraceParseResult parse_w3c_trace_context(std::string_view traceparent,
                                         std::string_view tracestate,
                                         uint16_t max_tracestate_len) noexcept;

/// \brief Format a TraceContext as a W3C traceparent header value.
///
/// \param[in] context The trace context to format.
/// \return A \c traceparent header value string.
std::string format_traceparent(const TraceContext& context);

/// \brief Format a TraceContext's tracestate for a W3C header.
///
/// \param[in] context The trace context to format.
/// \return A \c tracestate header value string.
std::string format_tracestate(const TraceContext& context);

} // namespace hpactor::tracing
