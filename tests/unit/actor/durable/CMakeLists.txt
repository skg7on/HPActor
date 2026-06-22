add_executable(test_unit_durable
    test_recovery_policy.cpp
    test_durable_behavior.cpp
    test_event_sourced_behavior.cpp
    test_passivation_manager.cpp
)
target_link_libraries(test_unit_durable hpactor hpactor_test_support GTest::gtest_main)
gtest_discover_tests(test_unit_durable)
