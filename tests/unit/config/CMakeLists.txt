add_executable(test_unit_config
    test_actor_factory_registry.cpp
)
target_link_libraries(test_unit_config hpactor hpactor_test_support GTest::gtest_main)
gtest_discover_tests(test_unit_config)
