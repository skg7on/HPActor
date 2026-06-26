# =============================================================================
# src/actor/CMakeLists.txt — Actor subsystem sources
# =============================================================================

target_sources(hpactor_lib PRIVATE
    abstract_actor.cpp
    actor_context.cpp
    actor_directory.cpp
    actor_route.cpp
    actor_system.cpp
    actor_type_registry.cpp
    ask_manager.cpp
    behavior.cpp
    blocking_actor.cpp
    daemon_actor.cpp
    event_based_actor.cpp
    http_gateway_actor.cpp
    local_actor.cpp
    scoped_actor.cpp
    spawn.cpp
    spawn_receiver.cpp
    durable/file_state_store.cpp
    durable/in_memory_state_store.cpp
    durable/passivation_manager.cpp
    lifecycle/lifecycle_actor.cpp
    lifecycle/passivation_manager.cpp
    lifecycle/quarantine_reason.cpp
    lifecycle/shutdown_coordinator.cpp
    receptionist/receptionist.cpp
    stream_receiver_actor.cpp
    stream_sender_actor.cpp
    routing/group_router.cpp
    routing/pool_router.cpp
    routing/routing_logic.cpp
)
