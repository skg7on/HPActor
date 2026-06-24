# =============================================================================
# src/fault/CMakeLists.txt — Fault injection subsystem sources
# =============================================================================

target_sources(hpactor_lib PRIVATE
    fault_controller.cpp
    fault_point_registry.cpp
    fault_points.cpp
    fault_schedule.cpp
)
