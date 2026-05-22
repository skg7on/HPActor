add_subdirectory(net)
if(ENABLE_EXAMPLES)
    add_subdirectory(examples)
endif()

# System tests — standalone executables exercising multiple subsystems
add_executable(test_system_topology_bootstrap
    test_system_topology_bootstrap.cpp)
target_link_libraries(test_system_topology_bootstrap hpactor)
target_compile_definitions(test_system_topology_bootstrap PRIVATE
    TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/tests/data/toml")
add_test(NAME test_system_topology_bootstrap
    COMMAND test_system_topology_bootstrap)

add_executable(test_system_runtime_workflow
    test_system_runtime_workflow.cpp)
target_link_libraries(test_system_runtime_workflow hpactor)
add_test(NAME test_system_runtime_workflow
    COMMAND test_system_runtime_workflow)

add_executable(test_system_supervision
    test_system_supervision.cpp)
target_link_libraries(test_system_supervision hpactor)
add_test(NAME test_system_supervision
    COMMAND test_system_supervision)

add_executable(test_system_backpressure
    test_system_backpressure.cpp)
target_link_libraries(test_system_backpressure hpactor)
add_test(NAME test_system_backpressure
    COMMAND test_system_backpressure)

add_executable(test_system_graceful_shutdown
    test_system_graceful_shutdown.cpp)
target_link_libraries(test_system_graceful_shutdown hpactor)
add_test(NAME test_system_graceful_shutdown
    COMMAND test_system_graceful_shutdown)
set_tests_properties(test_system_graceful_shutdown PROPERTIES TIMEOUT 30)

add_executable(test_system_observability
    test_system_observability.cpp)
target_link_libraries(test_system_observability hpactor)
add_test(NAME test_system_observability
    COMMAND test_system_observability)

add_executable(test_system_loopback_network
    test_system_loopback_network.cpp)
target_link_libraries(test_system_loopback_network hpactor hpactor_proto pthread)
add_test(NAME test_system_loopback_network
    COMMAND test_system_loopback_network)
set_tests_properties(test_system_loopback_network PROPERTIES TIMEOUT 30)

add_executable(test_system_order_platform
    test_system_order_platform.cpp)
target_link_libraries(test_system_order_platform hpactor)
target_include_directories(test_system_order_platform PRIVATE ${CMAKE_SOURCE_DIR})
add_test(NAME test_system_order_platform
    COMMAND test_system_order_platform)
