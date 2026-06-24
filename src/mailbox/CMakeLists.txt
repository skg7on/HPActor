# =============================================================================
# src/mailbox/CMakeLists.txt — Mailbox subsystem sources
# =============================================================================

target_sources(hpactor_lib PRIVATE
    backpressure_coordinator.cpp
    backpressure_signal_serialization.cpp
    dead_letter_queue.cpp
    delivery_pipeline.cpp
    delivery_result.cpp
    file_delivery_store.cpp
    in_memory_delivery_store.cpp
    local_delivery_engine.cpp
    memory_pressure_monitor.cpp
    outbound_tracker.cpp
)
