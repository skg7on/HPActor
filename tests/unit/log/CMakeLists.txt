add_executable(test_unit_log
    test_log_level.cpp
    test_log_category.cpp
    test_log_config.cpp
    test_log_formatter.cpp
    test_log_ring_buffer.cpp
    test_log_sinks.cpp
    test_syslog_sink.cpp
)
target_link_libraries(test_unit_log hpactor GTest::gtest_main)
gtest_discover_tests(test_unit_log)
