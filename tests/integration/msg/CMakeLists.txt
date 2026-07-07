add_executable(test_integration_msg
    test_msg_integration.cpp
)
target_link_libraries(test_integration_msg hpactor hpactor_proto pthread hpactor_test_support GTest::gtest_main)
gtest_discover_tests(test_integration_msg)
