add_executable(test_unit_mailbox
    test_message.cpp
    test_message_advanced.cpp
    test_mailbox_interface.cpp
    test_mailbox_factory.cpp
    test_mutex_mailbox.cpp
    test_mailbox_stress.cpp
    test_bounded_mailbox.cpp
    test_mailbox_overflow_policies.cpp
    test_mailbox_policy.cpp
    test_dead_letter_failure.cpp
)
target_link_libraries(test_unit_mailbox hpactor hpactor_test_support GTest::gtest_main)
gtest_discover_tests(test_unit_mailbox)
