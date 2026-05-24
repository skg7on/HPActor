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

enum class TraceExporterKind : uint8_t {
    kNoop,
    kMemory,
    kJsonFile,
    kOtlpHttp,
};

enum class SamplerKind : uint8_t {
    kAlwaysOff,
    kAlwaysOn,
    kTraceIdRatio,
    kParentBasedTraceIdRatio,
};

struct TraceConfig {
    bool enabled{false};
    bool propagate_unsampled{true};
    uint32_t ring_buffer_capacity{65536};
    std::string service_name{"hpactor"};
    SamplerKind sampler{SamplerKind::kParentBasedTraceIdRatio};
    double sample_ratio{0.01};
    TraceExporterKind exporter{TraceExporterKind::kOtlpHttp};
    std::string otlp_endpoint{"http://127.0.0.1:4318/v1/traces"};
    std::string json_file_path;
    std::chrono::milliseconds export_interval{500};
    uint32_t max_export_batch_size{512};
    uint16_t max_tracestate_len{256};
    bool record_actor_receive_spans{true};
    bool record_remote_producer_spans{true};
    bool record_local_producer_spans{false};
    bool record_payload_size{true};
    bool create_roots_for_actor_context_sends{false};
    bool create_roots_for_rpc{true};
    bool create_roots_for_http_ingress{true};
};

} // namespace hpactor::tracing