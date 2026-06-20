add_executable(test_unit_adt
    test_adt_mpsc_ring_buffer.cpp
    test_chaselev_deque.cpp
    test_multi_priority_work_queue.cpp
    test_calendar_queue.cpp
    test_stream_buffer.cpp
    test_adt_deep.cpp
)
target_link_libraries(test_unit_adt hpactor hpactor_test_support GTest::gtest_main)
gtest_discover_tests(test_unit_adt)
