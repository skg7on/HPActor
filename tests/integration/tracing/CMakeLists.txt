add_executable(test_integration_tracing
    test_trace_actor_context.cpp
    test_trace_actor_system.cpp
    test_trace_manager.cpp
    test_trace_message_propagation.cpp
    test_trace_exporters.cpp
    test_trace_rpc.cpp
    test_trace_wire_propagation.cpp
)
target_link_libraries(test_integration_tracing
    hpactor pthread hpactor_test_support GTest::gtest_main
)
gtest_discover_tests(test_integration_tracing PROPERTIES TIMEOUT 15)
