add_executable(test_integration_process
    test_daemon_integration.cpp
    test_process_lifecycle.cpp
)
target_link_libraries(test_integration_process hpactor hpactor_test_support GTest::gtest_main)
gtest_discover_tests(test_integration_process)
