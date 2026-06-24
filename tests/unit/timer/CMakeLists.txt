add_executable(test_unit_timer
    test_timing_wheel.cpp
    test_calendar_queue.cpp
)
target_link_libraries(test_unit_timer hpactor hpactor_test_support GTest::gtest_main)
gtest_discover_tests(test_unit_timer)
