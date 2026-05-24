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

enum class TraceParseStatus : uint8_t {
    kOk,
    kMissing,
    kMalformed,
    kUnsupportedVersion,
    kInvalidTraceId,
    kInvalidSpanId,
    kTracestateTooLarge,
};

struct TraceParseResult {
    TraceParseStatus status_value{TraceParseStatus::kMissing};
    TraceContext context{};

    [[nodiscard]] TraceParseStatus status() const noexcept {
        return status_value;
    }
    [[nodiscard]] bool ok() const noexcept {
        return status_value == TraceParseStatus::kOk;
    }
};

TraceParseResult parse_w3c_trace_context(std::string_view traceparent,
                                         std::string_view tracestate,
                                         uint16_t max_tracestate_len) noexcept;

std::string format_traceparent(const TraceContext& context);
std::string format_tracestate(const TraceContext& context);

} // namespace hpactor::tracing