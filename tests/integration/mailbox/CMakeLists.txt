add_executable(test_integration_mailbox
    test_mailbox_backpressure_stress.cpp
    test_dead_letter_queue.cpp
    test_reliable_messaging.cpp
)
target_link_libraries(test_integration_mailbox hpactor hpactor_proto pthread hpactor_test_support GTest::gtest_main)
gtest_discover_tests(test_integration_mailbox)
