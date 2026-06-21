# =============================================================================
# tests/unit/cluster/CMakeLists.txt — Cluster subsystem unit tests
# =============================================================================

add_executable(test_unit_cluster
    test_cluster_node_state.cpp
    test_cluster_fencing.cpp
    test_cluster_route_invalidation.cpp
    test_partition_policy.cpp
)
target_link_libraries(test_unit_cluster hpactor hpactor_proto pthread hpactor_test_support GTest::gtest_main)
gtest_discover_tests(test_unit_cluster)
