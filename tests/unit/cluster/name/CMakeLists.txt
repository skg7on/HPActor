add_executable(test_name_directory
    test_name_directory.cpp
)
target_link_libraries(test_name_directory PRIVATE hpactor_lib hpactor_cluster GTest::gtest GTest::gtest_main)
target_include_directories(test_name_directory PRIVATE ${CMAKE_SOURCE_DIR}/include)
gtest_discover_tests(test_name_directory
    TEST_PREFIX "unit/cluster/name/"
    DISCOVERY_TIMEOUT 10
)

add_executable(test_name_resolve_cache
    test_name_resolve_cache.cpp
)
target_link_libraries(test_name_resolve_cache PRIVATE hpactor_lib hpactor_cluster GTest::gtest GTest::gtest_main)
target_include_directories(test_name_resolve_cache PRIVATE ${CMAKE_SOURCE_DIR}/include)
gtest_discover_tests(test_name_resolve_cache
    TEST_PREFIX "unit/cluster/name/"
    DISCOVERY_TIMEOUT 10
)

add_executable(test_consistent_hash_ring
    test_consistent_hash_ring.cpp
)
target_link_libraries(test_consistent_hash_ring PRIVATE hpactor_lib hpactor_cluster GTest::gtest GTest::gtest_main)
target_include_directories(test_consistent_hash_ring PRIVATE ${CMAKE_SOURCE_DIR}/include)
gtest_discover_tests(test_consistent_hash_ring
    TEST_PREFIX "unit/cluster/name/"
    DISCOVERY_TIMEOUT 10
)

add_executable(test_name_resolver
    test_name_resolver.cpp
)
target_link_libraries(test_name_resolver PRIVATE hpactor_lib hpactor_cluster GTest::gtest GTest::gtest_main)
target_include_directories(test_name_resolver PRIVATE ${CMAKE_SOURCE_DIR}/include)
gtest_discover_tests(test_name_resolver
    TEST_PREFIX "unit/cluster/name/"
    DISCOVERY_TIMEOUT 10
)

add_executable(test_actor_directory_port
    test_actor_directory_port.cpp
)
target_link_libraries(test_actor_directory_port PRIVATE hpactor_lib GTest::gtest GTest::gtest_main)
target_include_directories(test_actor_directory_port PRIVATE ${CMAKE_SOURCE_DIR}/include)
gtest_discover_tests(test_actor_directory_port
    TEST_PREFIX "unit/cluster/name/"
    DISCOVERY_TIMEOUT 10
)
