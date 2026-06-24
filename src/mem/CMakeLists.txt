# =============================================================================
# src/mem/CMakeLists.txt — Memory management subsystem sources
# =============================================================================

target_sources(hpactor_lib PRIVATE
    compaction.cpp
    guard_page.cpp
    hibernation_manager.cpp
    memory_config.cpp
    memory_region.cpp
    memory_telemetry.cpp
    memory_tracker.cpp
    segment_provider.cpp
    slab_cache.cpp
    super_carrier.cpp
    thread_local_allocator.cpp
    zram.cpp
)
