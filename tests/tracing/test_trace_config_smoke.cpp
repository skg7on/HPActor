#include <hpactor/tracing/trace_config.hpp>

#include <cassert>

int main() {
    hpactor::tracing::TraceConfig cfg;
    assert(!cfg.enabled);
    assert(cfg.propagate_unsampled);
    assert(cfg.ring_buffer_capacity == 65536);
    assert(cfg.sampler == hpactor::tracing::SamplerKind::kParentBasedTraceIdRatio);
    assert(cfg.exporter == hpactor::tracing::TraceExporterKind::kOtlpHttp);
    assert(cfg.otlp_endpoint == "http://127.0.0.1:4318/v1/traces");
    return 0;
}
