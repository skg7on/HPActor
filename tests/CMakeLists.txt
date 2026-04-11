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

# test_types
add_executable(test_types test_types.cpp)
target_link_libraries(test_types hpactor)

# test_result
add_executable(test_result test_result.cpp)
target_link_libraries(test_result hpactor)

# test_actor_address
add_executable(test_actor_address test_actor_address.cpp)
target_link_libraries(test_actor_address hpactor)

# test_actor_ref
add_executable(test_actor_ref test_actor_ref.cpp)
target_link_libraries(test_actor_ref hpactor)

# test_abstract_actor
add_executable(test_abstract_actor test_abstract_actor.cpp)
target_link_libraries(test_abstract_actor hpactor)

# test_event_based_actor
add_executable(test_event_based_actor test_event_based_actor.cpp)
target_link_libraries(test_event_based_actor hpactor)

# test_blocking_actor
add_executable(test_blocking_actor test_blocking_actor.cpp)
target_link_libraries(test_blocking_actor hpactor)

# test_stateful_actor
add_executable(test_stateful_actor test_stateful_actor.cpp)
target_link_libraries(test_stateful_actor hpactor)

# test_actor_context
add_executable(test_actor_context test_actor_context.cpp)
target_link_libraries(test_actor_context hpactor)

# test_actor_system
add_executable(test_actor_system test_actor_system.cpp)
target_link_libraries(test_actor_system hpactor)

# test_actor_mailbox
add_executable(test_actor_mailbox test_actor_mailbox.cpp)
target_link_libraries(test_actor_mailbox hpactor)

# test_typed_actor
add_executable(test_typed_actor test_typed_actor.cpp)
target_link_libraries(test_typed_actor hpactor)

# test_supervision
add_executable(test_supervision test_supervision.cpp)
target_link_libraries(test_supervision hpactor)

# test_one_for_one_supervisor
add_executable(test_one_for_one_supervisor test_one_for_one_supervisor.cpp)
target_link_libraries(test_one_for_one_supervisor hpactor)

# test_all_for_one_supervisor
add_executable(test_all_for_one_supervisor test_all_for_one_supervisor.cpp)
target_link_libraries(test_all_for_one_supervisor hpactor)

# Register all tests with CTest
add_test(NAME test_message COMMAND test_message)
add_test(NAME test_message_advanced COMMAND test_message_advanced)
add_test(NAME test_mailbox_interface COMMAND test_mailbox_interface)
add_test(NAME test_mutex_mailbox COMMAND test_mutex_mailbox)
add_test(NAME test_mailbox_stress COMMAND test_mailbox_stress)
add_test(NAME test_mailbox_factory COMMAND test_mailbox_factory)
add_test(NAME test_types COMMAND test_types)
add_test(NAME test_result COMMAND test_result)
add_test(NAME test_actor_address COMMAND test_actor_address)
add_test(NAME test_actor_ref COMMAND test_actor_ref)
add_test(NAME test_abstract_actor COMMAND test_abstract_actor)
add_test(NAME test_event_based_actor COMMAND test_event_based_actor)
add_test(NAME test_blocking_actor COMMAND test_blocking_actor)
add_test(NAME test_stateful_actor COMMAND test_stateful_actor)
add_test(NAME test_actor_context COMMAND test_actor_context)
add_test(NAME test_actor_system COMMAND test_actor_system)
add_test(NAME test_actor_mailbox COMMAND test_actor_mailbox)
add_test(NAME test_typed_actor COMMAND test_typed_actor)
add_test(NAME test_supervision COMMAND test_supervision)
add_test(NAME test_one_for_one_supervisor COMMAND test_one_for_one_supervisor)
add_test(NAME test_all_for_one_supervisor COMMAND test_all_for_one_supervisor)