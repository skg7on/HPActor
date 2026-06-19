if(ENABLE_APPS)
    add_subdirectory(apps)
endif()

# System tests — single GTest executable exercising multiple subsystems
set(TEST_SYSTEM_SOURCES
    test_system_topology_bootstrap.cpp
    test_system_runtime_workflow.cpp
    test_system_supervision.cpp
    test_system_supervision_directives.cpp
    test_system_supervision_workflow.cpp
    test_system_backpressure.cpp
    test_system_graceful_shutdown.cpp
    test_system_observability.cpp
    test_system_loopback_network.cpp
    test_system_tcp_transport.cpp
    test_system_tls_transport.cpp
    test_system_registrar_server.cpp
    test_system_registrar_client.cpp
    test_system_udp_registrar.cpp
    test_system_gossip.cpp
    test_dlq_handoff.cpp
    test_system_network_workflow.cpp
    test_system_rpc_workflow.cpp
    test_system_http_api_workflow.cpp
    test_system_actor_lifecycle_workflow.cpp
    test_system_mailbox_scheduler_workflow.cpp
)

if(ENABLE_APPS)
    list(APPEND TEST_SYSTEM_SOURCES
        ${CMAKE_SOURCE_DIR}/apps/edgeops_telemetry/scenario.cpp
        test_system_order_platform.cpp
        test_system_edgeops_telemetry.cpp
    )
endif()

add_executable(test_system ${TEST_SYSTEM_SOURCES})
target_link_libraries(test_system hpactor hpactor_proto pthread hpactor_test_support GTest::gtest_main)
target_compile_definitions(test_system PRIVATE
    TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/tests/data/toml")
target_include_directories(test_system PRIVATE ${CMAKE_SOURCE_DIR})
gtest_discover_tests(test_system PROPERTIES TIMEOUT 30)
