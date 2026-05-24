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

#include <hpactor/tracing/json_exporter.hpp>
#include <hpactor/tracing/trace_context_parser.hpp>

#include <sstream>

namespace hpactor::tracing {

namespace {

std::string trace_id_hex(const TraceId& id) {
    TraceContext ctx;
    ctx.trace_id = id;
    ctx.span_id.bytes[7] = 1;
    std::string tp = format_traceparent(ctx);
    return tp.substr(3, 32);
}

std::string span_id_hex(const SpanId& id) {
    TraceContext ctx;
    ctx.trace_id.bytes[15] = 1;
    ctx.span_id = id;
    std::string tp = format_traceparent(ctx);
    return tp.substr(36, 16);
}

} // namespace

JsonFileExporter::JsonFileExporter(std::string path)
    : path_(std::move(path)), out_(path_, std::ios::app) {}

std::string span_record_to_json(const SpanRecord& record) {
    std::ostringstream os;
    os << R"({"trace_id":")" << trace_id_hex(record.trace_id)
       << R"(","span_id":")" << span_id_hex(record.span_id) << R"(","actor_id":)"
       << record.actor_id.value() << R"(,"type_tag":)" << record.type_tag
       << R"(,"start_ns":)" << record.start_ns << R"(,"end_ns":)"
       << record.end_ns << R"(,"kind":)" << static_cast<int>(record.kind)
       << R"(,"status":)" << static_cast<int>(record.status) << '}';
    return os.str();
}

result<void>
JsonFileExporter::export_batch(std::span<const SpanRecord> batch) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!out_.is_open()) {
        return result<void>::make(error(errors::unknown, "trace json file not "
                                                         "open"));
    }
    for (const auto& record : batch) {
        out_ << span_record_to_json(record) << '\n';
    }
    out_.flush();
    return result<void>::make();
}

void JsonFileExporter::shutdown() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (out_.is_open()) {
        out_.flush();
        out_.close();
    }
}

} // namespace hpactor::tracing