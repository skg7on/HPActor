add_executable(test_unit_cli
    test_lexer.cpp
    test_command_node.cpp
    test_formatters.cpp
    test_tabular_formatter.cpp
    test_json_formatter.cpp
    test_pager.cpp
    test_line_editor.cpp
    test_command_registry.cpp
    test_command_utils.cpp
    test_failure_commands.cpp
    test_misc_commands.cpp
    test_help_quit_commands.cpp
    test_actor_commands.cpp
    test_system_commands.cpp
    test_dlq_commands.cpp
    test_fault_commands.cpp
    test_memory_commands.cpp
    test_cli_session.cpp
    test_cli_server_actor.cpp
    test_cli_command_host.cpp
    test_cli_proto_server.cpp
    test_cli_http_server.cpp
)
target_include_directories(test_unit_cli PRIVATE ${PROJECT_SOURCE_DIR}/src/cli/commands)
target_link_libraries(test_unit_cli hpactor hpactor_test_support GTest::gtest_main)
gtest_discover_tests(test_unit_cli)

add_executable(test_unit_cli_wire_protocol
    test_cli_wire_protocol.cpp
)
target_link_libraries(test_unit_cli_wire_protocol hpactor GTest::gtest_main)
gtest_discover_tests(test_unit_cli_wire_protocol)
