# =============================================================================
# src/log/CMakeLists.txt — Structured logging subsystem sources
# =============================================================================

target_sources(hpactor_lib PRIVATE
    file_sink.cpp
    log_category.cpp
    log_drain.cpp
    log_formatter.cpp
    log_level.cpp
    log_manager.cpp
    logger.cpp
    rotating_file_sink.cpp
    stderr_sink.cpp
    syslog_sink.cpp
)
