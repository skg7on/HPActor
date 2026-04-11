# Tests CMakeLists.txt

# test_message
add_executable(test_message test_message.cpp)
target_link_libraries(test_message hpactor)

# test_message_advanced
add_executable(test_message_advanced test_message_advanced.cpp)
target_link_libraries(test_message_advanced hpactor)

# test_mailbox_interface
add_executable(test_mailbox_interface test_mailbox_interface.cpp)
target_link_libraries(test_mailbox_interface hpactor)

# test_mutex_mailbox
add_executable(test_mutex_mailbox test_mutex_mailbox.cpp)
target_link_libraries(test_mutex_mailbox hpactor)

# test_mailbox_stress
add_executable(test_mailbox_stress test_mailbox_stress.cpp)
target_link_libraries(test_mailbox_stress hpactor)

# test_mailbox_factory
add_executable(test_mailbox_factory test_mailbox_factory.cpp)
target_link_libraries(test_mailbox_factory hpactor)

# Register all tests with CTest
add_test(NAME test_message COMMAND test_message)
add_test(NAME test_message_advanced COMMAND test_message_advanced)
add_test(NAME test_mailbox_interface COMMAND test_mailbox_interface)
add_test(NAME test_mutex_mailbox COMMAND test_mutex_mailbox)
add_test(NAME test_mailbox_stress COMMAND test_mailbox_stress)
add_test(NAME test_mailbox_factory COMMAND test_mailbox_factory)