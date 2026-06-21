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
    singleton/test_singleton_identity.cpp
    singleton/test_singleton_election.cpp
)
target_link_libraries(test_unit_cluster hpactor hpactor_proto hpactor_cluster pthread hpactor_test_support GTest::gtest_main)
gtest_discover_tests(test_unit_cluster)
