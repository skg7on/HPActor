# =============================================================================
# src/metrics/CMakeLists.txt — Metrics subsystem sources
# =============================================================================

target_sources(hpactor_lib PRIVATE
    metrics_actor.cpp
    metrics_aggregator.cpp
    metrics_formatter.cpp
    metrics_registry.cpp
)
