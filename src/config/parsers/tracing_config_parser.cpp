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

#include <hpactor/config/toml_config_parser.hpp>
#include <hpactor/config/toml_parser_registry.hpp>
#include <hpactor/tracing/trace_config.hpp>

namespace hpactor::config {
namespace {

class TracingConfigParser final : public ITomlSystemConfigParser {
  public:
    static constexpr std::string_view kName = "system.tracing";
    static constexpr int kOrder = 90;

    std::string_view name() const noexcept override {
        return kName;
    }
    int order() const noexcept override {
        return kOrder;
    }

    result<void> parse(const TomlTableView& st, SystemDef& out,
                       TomlParseContext& /*ctx*/) const override {
        auto tracing = st.table("tracing");
        if (!tracing.valid()) {
            return result<void>::make();
        }

        auto& cfg = out.tracing;
        cfg.enabled = tracing.read_bool("enabled", false);
        cfg.service_name = tracing.read_string("service_name", "hpactor");
        cfg.propagate_unsampled = tracing.read_bool("propagate_unsampled", true);
        cfg.ring_buffer_capacity =
            tracing.read_uint32("ring_buffer_capacity", 65536);

        auto sampler_str = tracing.read_string("sampler", "parent_based_trace_"
                                                          "id_ratio");
        if (sampler_str == "always_off")
            cfg.sampler = hpactor::tracing::SamplerKind::kAlwaysOff;
        else if (sampler_str == "always_on")
            cfg.sampler = hpactor::tracing::SamplerKind::kAlwaysOn;
        else if (sampler_str == "trace_id_ratio")
            cfg.sampler = hpactor::tracing::SamplerKind::kTraceIdRatio;
        else
            cfg.sampler = hpactor::tracing::SamplerKind::kParentBasedTraceIdRatio;

        auto exporter_str = tracing.read_string("exporter", "otlp_http");
        if (exporter_str == "noop")
            cfg.exporter = hpactor::tracing::TraceExporterKind::kNoop;
        else if (exporter_str == "memory")
            cfg.exporter = hpactor::tracing::TraceExporterKind::kMemory;
        else if (exporter_str == "json_file")
            cfg.exporter = hpactor::tracing::TraceExporterKind::kJsonFile;
        else
            cfg.exporter = hpactor::tracing::TraceExporterKind::kOtlpHttp;

        cfg.otlp_endpoint = tracing.read_string("otlp_endpoint", "http://"
                                                                 "127.0.0.1:"
                                                                 "4318/v1/"
                                                                 "traces");
        cfg.json_file_path = tracing.read_string("json_file_path", "");
        cfg.export_interval = std::chrono::milliseconds(
            tracing.read_uint32("export_interval_ms", 500));
        cfg.max_export_batch_size =
            tracing.read_uint32("max_export_batch_size", 512);
        cfg.max_tracestate_len =
            static_cast<uint16_t>(tracing.read_uint32("max_tracestate_len", 256));
        cfg.record_actor_receive_spans =
            tracing.read_bool("record_actor_receive_spans", true);
        cfg.record_remote_producer_spans =
            tracing.read_bool("record_remote_producer_spans", true);
        cfg.record_local_producer_spans =
            tracing.read_bool("record_local_producer_spans", false);
        cfg.record_payload_size = tracing.read_bool("record_payload_size", true);
        cfg.create_roots_for_actor_context_sends =
            tracing.read_bool("create_roots_for_actor_context_sends", false);
        cfg.create_roots_for_rpc = tracing.read_bool("create_roots_for_rpc", true);
        cfg.create_roots_for_http_ingress =
            tracing.read_bool("create_roots_for_http_ingress", true);

        // Sample ratio
        cfg.sample_ratio = tracing.read_double("sample_ratio", 0.01);

        return result<void>::make();
    }
};

const TomlSystemParserRegistration<TracingConfigParser> kRegisterTracingConfigParser;

} // anonymous namespace
} // namespace hpactor::config