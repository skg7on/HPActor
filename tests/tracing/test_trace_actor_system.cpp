#include <hpactor/core/actor_system.hpp>

#include <cassert>

using namespace hpactor;

int main() {
    Config disabled;
    disabled.tracing.enabled = false;
    ActorSystem no_trace(disabled);
    assert(no_trace.trace_manager() == nullptr);

    Config enabled;
    enabled.tracing.enabled = true;
    enabled.tracing.exporter = tracing::TraceExporterKind::kMemory;
    enabled.tracing.sampler = tracing::SamplerKind::kAlwaysOn;
    ActorSystem with_trace(enabled);
    assert(with_trace.trace_manager() != nullptr);
    assert(with_trace.trace_manager()->enabled());
    return 0;
}
