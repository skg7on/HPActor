if(ENABLE_EXAMPLES)
    add_subdirectory(examples)
endif()

# System tests — single GTest executable exercising multiple subsystems
add_executable(test_system
    test_system_topology_bootstrap.cpp
    test_system_runtime_workflow.cpp
    test_system_supervision.cpp
    test_system_backpressure.cpp
    test_system_graceful_shutdown.cpp
    test_system_observability.cpp
    test_system_loopback_network.cpp
    test_system_order_platform.cpp
)
target_link_libraries(test_system hpactor hpactor_proto pthread hpactor_test_support GTest::gtest_main)
target_compile_definitions(test_system PRIVATE
    TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/tests/data/toml")
target_include_directories(test_system PRIVATE ${CMAKE_SOURCE_DIR})
gtest_discover_tests(test_system PROPERTIES TIMEOUT 30)
