# Tests CMakeLists.txt - organized by subsystem

# =============================================================================
# Core tests - types, result, serialization
# =============================================================================
add_executable(test_types core/test_types.cpp)
target_link_libraries(test_types hpactor)
add_test(NAME test_types COMMAND test_types)

add_executable(test_result core/test_result.cpp)
target_link_libraries(test_result hpactor)
add_test(NAME test_result COMMAND test_result)

add_executable(test_serializer core/test_serializer.cpp)
target_link_libraries(test_serializer hpactor)
add_test(NAME test_serializer COMMAND test_serializer)

# =============================================================================
# Mailbox tests
# =============================================================================
add_executable(test_message mailbox/test_message.cpp)
target_link_libraries(test_message hpactor)
add_test(NAME test_message COMMAND test_message)

add_executable(test_message_advanced mailbox/test_message_advanced.cpp)
target_link_libraries(test_message_advanced hpactor)
add_test(NAME test_message_advanced COMMAND test_message_advanced)

add_executable(test_mailbox_interface mailbox/test_mailbox_interface.cpp)
target_link_libraries(test_mailbox_interface hpactor)
add_test(NAME test_mailbox_interface COMMAND test_mailbox_interface)

add_executable(test_mailbox_factory mailbox/test_mailbox_factory.cpp)
target_link_libraries(test_mailbox_factory hpactor)
add_test(NAME test_mailbox_factory COMMAND test_mailbox_factory)

add_executable(test_mutex_mailbox mailbox/test_mutex_mailbox.cpp)
target_link_libraries(test_mutex_mailbox hpactor)
add_test(NAME test_mutex_mailbox COMMAND test_mutex_mailbox)

add_executable(test_mailbox_stress mailbox/test_mailbox_stress.cpp)
target_link_libraries(test_mailbox_stress hpactor)
add_test(NAME test_mailbox_stress COMMAND test_mailbox_stress)

# =============================================================================
# Actor tests
# =============================================================================
add_executable(test_abstract_actor actor/test_abstract_actor.cpp)
target_link_libraries(test_abstract_actor hpactor)
add_test(NAME test_abstract_actor COMMAND test_abstract_actor)

add_executable(test_event_based_actor actor/test_event_based_actor.cpp)
target_link_libraries(test_event_based_actor hpactor)
add_test(NAME test_event_based_actor COMMAND test_event_based_actor)

add_executable(test_blocking_actor actor/test_blocking_actor.cpp)
target_link_libraries(test_blocking_actor hpactor)
add_test(NAME test_blocking_actor COMMAND test_blocking_actor)

add_executable(test_stateful_actor actor/test_stateful_actor.cpp)
target_link_libraries(test_stateful_actor hpactor)
add_test(NAME test_stateful_actor COMMAND test_stateful_actor)

add_executable(test_typed_actor actor/test_typed_actor.cpp)
target_link_libraries(test_typed_actor hpactor)
add_test(NAME test_typed_actor COMMAND test_typed_actor)

add_executable(test_actor_context actor/test_actor_context.cpp)
target_link_libraries(test_actor_context hpactor)
add_test(NAME test_actor_context COMMAND test_actor_context)

add_executable(test_actor_system actor/test_actor_system.cpp)
target_link_libraries(test_actor_system hpactor)
add_test(NAME test_actor_system COMMAND test_actor_system)

add_executable(test_actor_mailbox actor/test_actor_mailbox.cpp)
target_link_libraries(test_actor_mailbox hpactor)
add_test(NAME test_actor_mailbox COMMAND test_actor_mailbox)

# =============================================================================
# Ref tests - actor references and addresses
# =============================================================================
add_executable(test_actor_address ref/test_actor_address.cpp)
target_link_libraries(test_actor_address hpactor)
add_test(NAME test_actor_address COMMAND test_actor_address)

add_executable(test_actor_proxy ref/test_actor_proxy.cpp)
target_link_libraries(test_actor_proxy hpactor)
add_test(NAME test_actor_proxy COMMAND test_actor_proxy)

# =============================================================================
# Net tests - network transport, connection, framing
# =============================================================================
add_executable(test_async_io_backend net/test_async_io_backend.cpp)
target_link_libraries(test_async_io_backend hpactor)
add_test(NAME test_async_io_backend COMMAND test_async_io_backend)

add_executable(test_event_loop net/test_event_loop.cpp)
target_link_libraries(test_event_loop hpactor)
add_test(NAME test_event_loop COMMAND test_event_loop)

add_executable(test_connection net/test_connection.cpp)
target_link_libraries(test_connection hpactor)
add_test(NAME test_connection COMMAND test_connection)

add_executable(test_frame net/test_frame.cpp)
target_link_libraries(test_frame hpactor)
add_test(NAME test_frame COMMAND test_frame)

add_executable(test_tls_context net/test_tls_context.cpp)
target_link_libraries(test_tls_context hpactor)
add_test(NAME test_tls_context COMMAND test_tls_context)

add_executable(test_tls_connection net/test_tls_connection.cpp)
target_link_libraries(test_tls_connection hpactor)
add_test(NAME test_tls_connection COMMAND test_tls_connection)

add_executable(test_connection_pool net/test_connection_pool.cpp)
target_link_libraries(test_connection_pool hpactor)
add_test(NAME test_connection_pool COMMAND test_connection_pool)

add_executable(test_tls_integration net/test_tls_integration.cpp)
target_link_libraries(test_tls_integration hpactor)
add_test(NAME test_tls_integration COMMAND test_tls_integration)

add_executable(test_registrar net/test_registrar.cpp)
target_link_libraries(test_registrar hpactor)
add_test(NAME test_registrar COMMAND test_registrar)

# =============================================================================
# Spawn tests - remote actor spawn
# =============================================================================
add_subdirectory(spawn)

# =============================================================================
# RPC tests - RPC channel
# =============================================================================
add_executable(test_rpc_channel rpc/test_rpc_channel.cpp)
target_link_libraries(test_rpc_channel hpactor)
add_test(NAME test_rpc_channel COMMAND test_rpc_channel)

# =============================================================================
# Supervision tests
# =============================================================================
add_executable(test_supervision supervision/test_supervision.cpp)
target_link_libraries(test_supervision hpactor)
add_test(NAME test_supervision COMMAND test_supervision)

add_executable(test_one_for_one_supervisor supervision/test_one_for_one_supervisor.cpp)
target_link_libraries(test_one_for_one_supervisor hpactor)
add_test(NAME test_one_for_one_supervisor COMMAND test_one_for_one_supervisor)

add_executable(test_all_for_one_supervisor supervision/test_all_for_one_supervisor.cpp)
target_link_libraries(test_all_for_one_supervisor hpactor)
add_test(NAME test_all_for_one_supervisor COMMAND test_all_for_one_supervisor)

add_executable(test_supervisor_actor supervision/test_supervisor_actor.cpp)
target_link_libraries(test_supervisor_actor hpactor)
add_test(NAME test_supervisor_actor COMMAND test_supervisor_actor)

add_executable(test_self_supervising_actor supervision/test_self_supervising_actor.cpp)
target_link_libraries(test_self_supervising_actor hpactor)
add_test(NAME test_self_supervising_actor COMMAND test_self_supervising_actor)

# =============================================================================
# Scheduling tests
# =============================================================================
add_executable(test_chaselev_deque sched/test_chaselev_deque.cpp)
target_link_libraries(test_chaselev_deque hpactor)
add_test(NAME test_chaselev_deque COMMAND test_chaselev_deque)

add_executable(test_multi_priority_work_queue sched/test_multi_priority_work_queue.cpp)
target_link_libraries(test_multi_priority_work_queue hpactor)
add_test(NAME test_multi_priority_work_queue COMMAND test_multi_priority_work_queue)

add_executable(test_hybrid_scheduler sched/test_hybrid_scheduler.cpp)
target_link_libraries(test_hybrid_scheduler hpactor)
add_test(NAME test_hybrid_scheduler COMMAND test_hybrid_scheduler)

add_executable(test_edf_queue sched/test_edf_queue.cpp)
target_link_libraries(test_edf_queue hpactor)
add_test(NAME test_edf_queue COMMAND test_edf_queue)

add_executable(test_a2ws sched/test_a2ws.cpp)
target_link_libraries(test_a2ws hpactor)
add_test(NAME test_a2ws COMMAND test_a2ws)

add_executable(test_priority_scheduler sched/test_priority_scheduler.cpp)
target_link_libraries(test_priority_scheduler hpactor)
add_test(NAME test_priority_scheduler COMMAND test_priority_scheduler)

add_executable(test_mpsc_actor_mailbox sched/test_actor_mailbox.cpp)
target_link_libraries(test_mpsc_actor_mailbox hpactor)
add_test(NAME test_mpsc_actor_mailbox COMMAND test_mpsc_actor_mailbox)

add_executable(test_coroutine_task sched/test_coroutine_task.cpp)
target_link_libraries(test_coroutine_task hpactor)
add_test(NAME test_coroutine_task COMMAND test_coroutine_task)

add_executable(test_actor_state sched/test_actor_state.cpp)
target_link_libraries(test_actor_state hpactor)
add_test(NAME test_actor_state COMMAND test_actor_state)

add_executable(test_mailbox_awaiter sched/test_mailbox_awaiter.cpp)
target_link_libraries(test_mailbox_awaiter hpactor)
add_test(NAME test_mailbox_awaiter COMMAND test_mailbox_awaiter)

add_executable(test_coroutine_scheduling sched/test_coroutine_scheduling.cpp)
target_link_libraries(test_coroutine_scheduling hpactor)
add_test(NAME test_coroutine_scheduling COMMAND test_coroutine_scheduling)
