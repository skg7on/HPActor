add_executable(test_integration_fault
    test_fault_mailbox.cpp
    test_fault_seed_replay.cpp
)
target_link_libraries(test_integration_fault hpactor hpactor_proto pthread hpactor_test_support GTest::gtest_main)
gtest_discover_tests(test_integration_fault)
