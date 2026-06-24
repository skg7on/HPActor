# =============================================================================
# src/process/CMakeLists.txt — Process management subsystem sources
# =============================================================================

target_sources(hpactor_lib PRIVATE
    health_check.cpp
    health_http_server.cpp
    process_manager.cpp
    watchdog_actor.cpp
)
