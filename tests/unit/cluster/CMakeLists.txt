# =============================================================================
# tests/unit/cluster/CMakeLists.txt — Cluster subsystem unit tests
# =============================================================================

add_executable(test_unit_cluster
    test_cluster_node_state.cpp
    test_cluster_fencing.cpp
    test_cluster_route_invalidation.cpp
    test_partition_policy.cpp
    sharding/test_shard_resolver.cpp
    sharding/test_shard_table.cpp
    sharding/test_placement_strategy.cpp
    sharding/test_shard_handoff.cpp
    sharding/test_shard_coordinator.cpp
    sharding/test_shard_coordinator_token.cpp
    sharding/test_shard_coordinator_actor.cpp
    singleton/test_leadership_lease.cpp
    singleton/test_leadership_status.cpp
    singleton/test_singleton_identity.cpp
    singleton/test_singleton_election.cpp
    singleton/test_majority_based_election.cpp
    singleton/test_fixed_priority_election.cpp
    singleton/test_singleton_manager.cpp
    singleton/test_singleton_manager_leader.cpp
    singleton/test_singleton_manager_actor.cpp
    singleton/test_fake_leadership_backend.cpp
    singleton/test_leadership_backend_adapter.cpp
    singleton/test_etcd_serialize.cpp
    singleton/test_etcd_leadership_backend.cpp
    pubsub/test_pubsub_mediator_core.cpp
    pubsub/test_pubsub_mediator_actor.cpp
    receptionist/test_cluster_receptionist_core.cpp
    receptionist/test_cluster_receptionist_actor.cpp
)
target_link_libraries(test_unit_cluster hpactor hpactor_proto hpactor_cluster hpactor_etcd pthread hpactor_test_support GTest::gtest_main)
gtest_discover_tests(test_unit_cluster)

add_subdirectory(name)
