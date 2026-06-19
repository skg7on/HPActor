add_executable(test_integration_cli
    test_cli_actor.cpp
    test_cli_commands_workflow.cpp
    test_cli_deep_workflow.cpp
    test_cli_integration.cpp
    test_cli_server_proto.cpp
    test_dlq_integration.cpp
)
target_link_libraries(test_integration_cli hpactor hpactor_test_support GTest::gtest_main)
gtest_discover_tests(test_integration_cli)
