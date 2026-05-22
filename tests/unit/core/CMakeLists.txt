add_executable(test_unit_core
    test_smoke.cpp
)
target_link_libraries(test_unit_core hpactor hpactor_test_support GTest::gtest_main)
gtest_discover_tests(test_unit_core)
