add_executable(test_unit_process
    test_process_manager.cpp
    test_watchdog_actor.cpp
    test_health_http.cpp
)
target_link_libraries(test_unit_process hpactor GTest::gtest_main)
gtest_discover_tests(test_unit_process)
