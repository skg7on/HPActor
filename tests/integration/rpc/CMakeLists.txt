add_executable(test_integration_rpc
    test_rpc_channel.cpp
    test_rpc_deadline.cpp
    test_rpc_branches.cpp
)
target_link_libraries(test_integration_rpc
    hpactor hpactor_test_support GTest::gtest_main
)
gtest_discover_tests(test_integration_rpc)
