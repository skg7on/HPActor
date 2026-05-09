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

add_executable(test_daemon_actor actor/test_daemon_actor.cpp)
target_link_libraries(test_daemon_actor hpactor)
add_test(NAME test_daemon_actor COMMAND test_daemon_actor)

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

add_executable(test_proto_stateful_actor actor/test_proto_stateful_actor.cpp)
target_link_libraries(test_proto_stateful_actor hpactor)
add_test(NAME test_proto_stateful_actor COMMAND test_proto_stateful_actor)

add_executable(test_proto_registry actor/test_proto_registry.cpp)
target_link_libraries(test_proto_registry hpactor hpactor_proto pthread)
add_test(NAME test_proto_registry COMMAND test_proto_registry)

add_executable(test_actor_ref_cache actor/test_actor_ref_cache.cpp)
target_link_libraries(test_actor_ref_cache hpactor)
add_test(NAME test_actor_ref_cache COMMAND test_actor_ref_cache)

add_executable(test_unified_message_passing actor/test_unified_message_passing.cpp)
target_link_libraries(test_unified_message_passing hpactor hpactor_proto pthread)
add_test(NAME test_unified_message_passing COMMAND test_unified_message_passing)

add_executable(test_link_monitor actor/test_link_monitor.cpp)
target_link_libraries(test_link_monitor hpactor hpactor_proto pthread)
add_test(NAME test_link_monitor COMMAND test_link_monitor)

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

add_executable(test_communication_endpoint net/test_communication_endpoint.cpp)
target_link_libraries(test_communication_endpoint hpactor)
add_test(NAME test_communication_endpoint COMMAND test_communication_endpoint)

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

add_executable(test_registrar_connection net/test_registrar_connection.cpp)
target_link_libraries(test_registrar_connection hpactor)
add_test(NAME test_registrar_connection COMMAND test_registrar_connection)

add_executable(test_registrar_serialization net/test_registrar_serialization.cpp)
target_link_libraries(test_registrar_serialization hpactor_lib hpactor_proto pthread)
add_test(NAME test_registrar_serialization COMMAND test_registrar_serialization)

add_executable(test_unix_domain_socket net/test_unix_domain_socket.cpp)
target_link_libraries(test_unix_domain_socket hpactor)
add_test(NAME test_unix_domain_socket COMMAND test_unix_domain_socket)

add_executable(test_uds_integration net/test_uds_integration.cpp)
target_link_libraries(test_uds_integration hpactor)
add_test(NAME test_uds_integration COMMAND test_uds_integration)

add_executable(test_tcp_transport_comprehensive net/test_tcp_transport_comprehensive.cpp)
target_link_libraries(test_tcp_transport_comprehensive hpactor)
add_test(NAME test_tcp_transport_comprehensive COMMAND test_tcp_transport_comprehensive)

add_executable(test_reactor_dispatcher net/test_reactor_dispatcher.cpp)
target_link_libraries(test_reactor_dispatcher hpactor)
add_test(NAME test_reactor_dispatcher COMMAND test_reactor_dispatcher)

add_executable(test_metrics_registry metrics/test_metrics_registry.cpp)
target_link_libraries(test_metrics_registry hpactor pthread)
add_test(NAME test_metrics_registry COMMAND test_metrics_registry)

add_executable(test_metrics_integration metrics/test_metrics_integration.cpp)
target_link_libraries(test_metrics_integration hpactor pthread)
add_test(NAME test_metrics_integration COMMAND test_metrics_integration)

add_executable(test_log_level log/test_log_level.cpp)
target_link_libraries(test_log_level hpactor)
add_test(NAME test_log_level COMMAND test_log_level)

add_executable(test_log_category log/test_log_category.cpp)
target_link_libraries(test_log_category hpactor)
add_test(NAME test_log_category COMMAND test_log_category)

add_executable(test_proactor_dispatcher net/test_proactor_dispatcher.cpp)
target_link_libraries(test_proactor_dispatcher hpactor)
add_test(NAME test_proactor_dispatcher COMMAND test_proactor_dispatcher)

add_executable(test_gossip_membership net/test_gossip_membership.cpp)
target_link_libraries(test_gossip_membership hpactor)
add_test(NAME test_gossip_membership COMMAND test_gossip_membership)

add_executable(test_service_discovery net/test_service_discovery.cpp)
target_link_libraries(test_service_discovery hpactor)
add_test(NAME test_service_discovery COMMAND test_service_discovery)

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
# Config tests
# =============================================================================
add_executable(test_actor_factory_registry config/test_actor_factory_registry.cpp)
target_link_libraries(test_actor_factory_registry hpactor)
add_test(NAME test_actor_factory_registry COMMAND test_actor_factory_registry)

add_executable(test_toml_parser config/test_toml_parser.cpp)
target_link_libraries(test_toml_parser hpactor)
target_compile_definitions(test_toml_parser PRIVATE
    TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/tests/data/toml")
add_test(NAME test_toml_parser COMMAND test_toml_parser)

add_executable(test_bootstrap_engine config/test_bootstrap_engine.cpp)
target_link_libraries(test_bootstrap_engine hpactor)
add_test(NAME test_bootstrap_engine COMMAND test_bootstrap_engine)

add_executable(test_binary_roundtrip config/test_binary_roundtrip.cpp)
target_link_libraries(test_binary_roundtrip hpactor)
target_compile_definitions(test_binary_roundtrip PRIVATE
    TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/tests/data/toml")
add_test(NAME test_binary_roundtrip COMMAND test_binary_roundtrip)

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

add_executable(test_dedicated_thread_pool sched/test_dedicated_thread_pool.cpp)
target_link_libraries(test_dedicated_thread_pool hpactor)
add_test(NAME test_dedicated_thread_pool COMMAND test_dedicated_thread_pool)

add_executable(test_dispatch_policy sched/test_dispatch_policy.cpp)
target_link_libraries(test_dispatch_policy hpactor)
add_test(NAME test_dispatch_policy COMMAND test_dispatch_policy)

add_executable(test_calendar_queue sched/test_calendar_queue.cpp)
target_link_libraries(test_calendar_queue hpactor)
add_test(NAME test_calendar_queue COMMAND test_calendar_queue)

# =============================================================================
# HTTP tests
# =============================================================================
add_executable(test_http_parser net/test_http_parser.cpp)
target_link_libraries(test_http_parser hpactor)
add_test(NAME test_http_parser COMMAND test_http_parser)

add_executable(test_http_serializer net/test_http_serializer.cpp)
target_link_libraries(test_http_serializer hpactor)
add_test(NAME test_http_serializer COMMAND test_http_serializer)

add_executable(test_http_connection net/test_http_connection.cpp)
target_link_libraries(test_http_connection hpactor)
add_test(NAME test_http_connection COMMAND test_http_connection)

add_executable(test_http_gateway net/test_http_gateway.cpp)
target_link_libraries(test_http_gateway hpactor)
add_test(NAME test_http_gateway COMMAND test_http_gateway)
set_tests_properties(test_http_gateway PROPERTIES TIMEOUT 15)

# =============================================================================
# Memory tests
# =============================================================================
add_executable(test_size_class mem/test_size_class.cpp)
target_link_libraries(test_size_class hpactor)
add_test(NAME test_size_class COMMAND test_size_class)

add_executable(test_alloc_header mem/test_alloc_header.cpp)
target_link_libraries(test_alloc_header hpactor)
add_test(NAME test_alloc_header COMMAND test_alloc_header)

add_executable(test_freelist mem/test_freelist.cpp)
target_link_libraries(test_freelist hpactor pthread)
add_test(NAME test_freelist COMMAND test_freelist)

add_executable(test_segment_provider mem/test_segment_provider.cpp)
target_link_libraries(test_segment_provider hpactor pthread)
add_test(NAME test_segment_provider COMMAND test_segment_provider)

add_executable(test_slab_cache mem/test_slab_cache.cpp)
target_link_libraries(test_slab_cache hpactor pthread)
add_test(NAME test_slab_cache COMMAND test_slab_cache)

add_executable(test_thread_local_allocator mem/test_thread_local_allocator.cpp)
target_link_libraries(test_thread_local_allocator hpactor pthread)
add_test(NAME test_thread_local_allocator COMMAND test_thread_local_allocator)

add_executable(test_memory_stress mem/test_memory_stress.cpp)
target_link_libraries(test_memory_stress hpactor pthread)
add_test(NAME test_memory_stress COMMAND test_memory_stress)

add_executable(test_memory_tracker mem/test_memory_tracker.cpp)
target_link_libraries(test_memory_tracker hpactor pthread)
add_test(NAME test_memory_tracker COMMAND test_memory_tracker)

add_executable(test_telemetry_ring_buffer mem/test_telemetry_ring_buffer.cpp)
target_link_libraries(test_telemetry_ring_buffer hpactor pthread)
add_test(NAME test_telemetry_ring_buffer COMMAND test_telemetry_ring_buffer)

add_executable(test_memory_poisoning mem/test_memory_poisoning.cpp)
target_link_libraries(test_memory_poisoning hpactor pthread)
add_test(NAME test_memory_poisoning COMMAND test_memory_poisoning)

add_executable(test_hibernation mem/test_hibernation.cpp)
target_link_libraries(test_hibernation hpactor pthread)
add_test(NAME test_hibernation COMMAND test_hibernation)

add_executable(test_guard_page mem/test_guard_page.cpp)
target_link_libraries(test_guard_page hpactor pthread)
add_test(NAME test_guard_page COMMAND test_guard_page)

add_executable(test_compaction mem/test_compaction.cpp)
target_link_libraries(test_compaction hpactor pthread)
add_test(NAME test_compaction COMMAND test_compaction)

add_executable(test_allocator_benchmark mem/test_allocator_benchmark.cpp)
target_link_libraries(test_allocator_benchmark hpactor pthread)
add_test(NAME test_allocator_benchmark COMMAND test_allocator_benchmark)

add_executable(test_std_allocator mem/test_std_allocator.cpp)
target_link_libraries(test_std_allocator hpactor pthread)
add_test(NAME test_std_allocator COMMAND test_std_allocator)

# =============================================================================
# CLI tests
# =============================================================================
if(ENABLE_CLI)
    add_executable(test_lexer cli/test_lexer.cpp)
    target_link_libraries(test_lexer hpactor)
    add_test(NAME test_lexer COMMAND test_lexer)

    add_executable(test_command_node cli/test_command_node.cpp)
    target_link_libraries(test_command_node hpactor)
    add_test(NAME test_command_node COMMAND test_command_node)

    add_executable(test_formatters cli/test_formatters.cpp)
    target_link_libraries(test_formatters hpactor)
    add_test(NAME test_formatters COMMAND test_formatters)

    add_executable(test_pager cli/test_pager.cpp)
    target_link_libraries(test_pager hpactor)
    add_test(NAME test_pager COMMAND test_pager)

    add_executable(test_cli_integration cli/test_cli_integration.cpp)
    target_link_libraries(test_cli_integration hpactor)
    add_test(NAME test_cli_integration COMMAND test_cli_integration)

    add_executable(test_line_editor cli/test_line_editor.cpp)
    target_link_libraries(test_line_editor hpactor)
    add_test(NAME test_line_editor COMMAND test_line_editor)
endif()
