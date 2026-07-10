# =============================================================================
# src/cluster/CMakeLists.txt — Cluster subsystem (failure model, partitioning,
# route invalidation)
# =============================================================================

add_library(hpactor_cluster STATIC
    cluster_system_bridge.cpp
    cluster_failure_model.cpp
    cluster_runtime_impl.cpp
    route_invalidation.cpp
    sharding/shard_resolver.cpp
    sharding/shard_table.cpp
    sharding/static_placement.cpp
    sharding/rendezvous_hash.cpp
    sharding/shard_handoff.cpp
    sharding/shard_coordinator.cpp
    singleton/oldest_node_election.cpp
    singleton/majority_based_election.cpp
    singleton/fixed_priority_election.cpp
    singleton/singleton_manager.cpp
    singleton/leadership_lease.cpp
    singleton/leadership_status.cpp
    singleton/singleton_manager_actor.cpp
    sharding/shard_coordinator_actor.cpp
    pubsub/pubsub_mediator_core.cpp
    pubsub/pubsub_mediator_actor.cpp
    receptionist/cluster_receptionist_core.cpp
    receptionist/cluster_receptionist_actor.cpp
)
target_link_libraries(hpactor_cluster PUBLIC hpactor)
target_include_directories(hpactor_cluster PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)
