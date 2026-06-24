# =============================================================================
# src/timer/CMakeLists.txt — Timer subsystem sources
# =============================================================================

target_sources(hpactor_lib PRIVATE
    calendar_queue.cpp
    timer_plane_shard.cpp
    timing_wheel.cpp
)
