add_executable(test_integration_sched
    test_priority_scheduler.cpp
    test_worker_thread.cpp
    test_dedicated_thread_pool.cpp
    test_coroutine_scheduling.cpp
)
target_link_libraries(test_integration_sched hpactor pthread hpactor_test_support GTest::gtest_main)
gtest_discover_tests(test_integration_sched PROPERTIES TIMEOUT 15)
