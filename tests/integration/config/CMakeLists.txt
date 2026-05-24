add_executable(test_integration_config
    test_toml_parser.cpp
    test_toml_parser_registry.cpp
    test_tracing_config_parser.cpp
    test_bootstrap_engine.cpp
    test_mailbox_config.cpp
    test_shutdown_config.cpp
    test_binary_roundtrip.cpp
)
target_compile_definitions(test_integration_config PRIVATE TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/tests/data/toml")
target_link_libraries(test_integration_config hpactor hpactor_test_support GTest::gtest_main)
gtest_discover_tests(test_integration_config PROPERTIES TIMEOUT 15)
