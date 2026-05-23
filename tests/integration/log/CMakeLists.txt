add_executable(test_integration_log
    test_log_integration.cpp
)
target_link_libraries(test_integration_log
    hpactor pthread hpactor_test_support GTest::gtest_main
)
gtest_discover_tests(test_integration_log)
