add_executable(test_unit_fault
    test_fault_controller.cpp
    test_fault_macro.cpp
    test_fault_point.cpp
    test_fault_schedule.cpp
    test_passivation_fault_points.cpp
)
target_link_libraries(test_unit_fault hpactor hpactor_test_support GTest::gtest_main)
gtest_discover_tests(test_unit_fault)
