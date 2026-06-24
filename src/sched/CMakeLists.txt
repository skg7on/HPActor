# =============================================================================
# src/sched/CMakeLists.txt — Scheduler subsystem sources
# =============================================================================

target_sources(hpactor_lib PRIVATE
    a2ws.cpp
    actor_execution_engine.cpp
    actor_ready_gate.cpp
    dedicated_thread_pool.cpp
    edf_queue.cpp
    scheduler.cpp
    work_placement_scheduler.cpp
    worker_thread.cpp
)
