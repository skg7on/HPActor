add_executable(test_integration_metrics
    test_metrics_registry.cpp
    test_metrics_aggregator.cpp
    test_metrics_integration.cpp
)
target_link_libraries(test_integration_metrics hpactor pthread hpactor_test_support GTest::gtest_main)
gtest_discover_tests(test_integration_metrics)
