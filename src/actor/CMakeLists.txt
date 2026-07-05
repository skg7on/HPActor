# =============================================================================
# src/actor/CMakeLists.txt — Actor subsystem sources
# =============================================================================

target_sources(hpactor_lib PRIVATE
    # Core actor types (flat — public API surface)
    abstract_actor.cpp
    actor_context.cpp
    behavior.cpp
    blocking_actor.cpp
    daemon_actor.cpp
    event_based_actor.cpp
    local_actor.cpp
    scoped_actor.cpp

    # System infrastructure
    system/actor_directory.cpp
    system/actor_route.cpp
    system/actor_system.cpp

    # Spawn subsystem
    spawn/actor_type_registry.cpp
    spawn/spawn.cpp
    spawn/spawn_receiver.cpp

    # Stream subsystem
    stream/stream_receiver_actor.cpp
    stream/stream_registry.cpp
    stream/stream_runtime.cpp
    stream/stream_sender_actor.cpp

    # Gateway subsystem
    gateway/http_gateway_actor.cpp

    # Request-response subsystem
    request/ask_manager.cpp

    # Durable state
    durable/file_state_store.cpp
    durable/in_memory_state_store.cpp
    durable/passivation_manager.cpp

    # Lifecycle
    lifecycle/lifecycle_actor.cpp
    lifecycle/passivation_manager.cpp
    lifecycle/quarantine_reason.cpp
    lifecycle/shutdown_coordinator.cpp

    # Receptionist
    receptionist/receptionist.cpp

    # Routing
    routing/group_router.cpp
    routing/pool_router.cpp
    routing/routing_logic.cpp
)
