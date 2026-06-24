# =============================================================================
# src/tracing/CMakeLists.txt — Distributed tracing subsystem sources
# =============================================================================

target_sources(hpactor_lib PRIVATE
    json_exporter.cpp
    memory_exporter.cpp
    otlp_exporter.cpp
    sampler.cpp
    trace_context_parser.cpp
    trace_manager.cpp
)
