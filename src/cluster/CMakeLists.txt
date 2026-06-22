# =============================================================================
# src/cluster/CMakeLists.txt — Cluster subsystem (failure model, partitioning,
# route invalidation)
# =============================================================================

add_library(hpactor_cluster STATIC
    cluster_system_bridge.cpp
    cluster_failure_model.cpp
    route_invalidation.cpp
    sharding/shard_resolver.cpp
    sharding/shard_table.cpp
    sharding/static_placement.cpp
    sharding/rendezvous_hash.cpp
    sharding/shard_handoff.cpp
    sharding/shard_coordinator.cpp
    singleton/oldest_node_election.cpp
    singleton/singleton_manager.cpp
    singleton/singleton_manager_actor.cpp
    sharding/shard_coordinator_actor.cpp
)
target_link_libraries(hpactor_cluster PUBLIC hpactor)
target_include_directories(hpactor_cluster PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)
