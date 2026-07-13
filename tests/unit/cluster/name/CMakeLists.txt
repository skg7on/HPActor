add_executable(test_name_directory
    test_name_directory.cpp
)
target_link_libraries(test_name_directory PRIVATE hpactor_lib hpactor_cluster GTest::gtest GTest::gtest_main)
target_include_directories(test_name_directory PRIVATE ${CMAKE_SOURCE_DIR}/include)
gtest_discover_tests(test_name_directory
    TEST_PREFIX "unit/cluster/name/"
    DISCOVERY_TIMEOUT 10
)
