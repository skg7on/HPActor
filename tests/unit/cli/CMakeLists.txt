add_executable(test_unit_cli
    test_lexer.cpp
    test_command_node.cpp
    test_formatters.cpp
    test_tabular_formatter.cpp
    test_json_formatter.cpp
    test_pager.cpp
    test_line_editor.cpp
    test_command_registry.cpp
)
target_link_libraries(test_unit_cli hpactor hpactor_test_support GTest::gtest_main)
gtest_discover_tests(test_unit_cli)
