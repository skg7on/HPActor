# =============================================================================
# src/CMakeLists.txt — hpactor_lib shared library and platform backends
# =============================================================================

add_library(hpactor_lib SHARED
    actor/abstract_actor.cpp
    actor/actor_context.cpp
    actor/actor_directory.cpp
    mailbox/local_delivery_engine.cpp
    mailbox/backpressure_coordinator.cpp
    mailbox/delivery_pipeline.cpp
    actor/lifecycle/shutdown_coordinator.cpp
    actor/actor_system.cpp
    actor/ask_manager.cpp
    actor/daemon_actor.cpp
    actor/behavior.cpp
    actor/event_based_actor.cpp
    actor/local_actor.cpp
    actor/spawn_receiver.cpp
    actor/http_gateway_actor.cpp
    actor/lifecycle/lifecycle_actor.cpp
    actor/blocking_actor.cpp
    actor/durable/in_memory_state_store.cpp
    actor/durable/file_state_store.cpp
    actor/actor_route.cpp
    actor/routing/routing_logic.cpp
    actor/routing/pool_router.cpp
    actor/routing/group_router.cpp
    actor/receptionist/receptionist.cpp
    mailbox/memory_pressure_monitor.cpp
    actor/lifecycle/passivation_manager.cpp
    actor/scoped_actor.cpp
    actor/actor_type_registry.cpp
    actor/spawn.cpp
    ref/actor_proxy.cpp
    ref/actor_ref.cpp
    supervision/supervision.cpp
    mailbox/dead_letter_queue.cpp
    mailbox/delivery_result.cpp
    adt/dedup_cache.cpp
    adt/json_helpers.cpp
    mailbox/backpressure_signal_serialization.cpp
    config/actor_factory_registry.cpp
    config/toml_parser.cpp
    config/toml_table_view.cpp
    config/toml_parser_registry.cpp
    config/parsers/system_core_config_parser.cpp
    config/parsers/metrics_config_parser.cpp
    config/parsers/logging_config_parser.cpp
    config/parsers/passivation_config_parser.cpp
    config/parsers/cli_config_parser.cpp
    config/parsers/discovery_config_parser.cpp
    config/parsers/topology_config_parser.cpp
    config/parsers/mailbox_config_parser.cpp
    config/parsers/dead_letters_config_parser.cpp
    config/parsers/tracing_config_parser.cpp
    config/parsers/shutdown_config_parser.cpp
    config/parsers/delivery_config_parser.cpp
    config/parsers/quarantine_parser.cpp
    config/parsers/process_config_parser.cpp
    config/binary_serializer.cpp
    config/binary_loader.cpp
    core/proto_type_registry.cpp
    types/typed_message.cpp
    types/failure_reason.cpp
    actor/lifecycle/quarantine_reason.cpp
    net/event_loop.cpp
    net/acceptor.cpp
    net/connection.cpp
    net/tcp_transport.cpp
    msg/frame.cpp
    msg/frame_protobuf.cpp
    msg/delivery_receipt.cpp
    msg/outbound_delivery_tracker.cpp
    net/tls_context.cpp
    net/tls_connection.cpp
    net/wireframe_connection.cpp
    net/connection_pool.cpp
    net/endpoint.cpp
    net/registrar.cpp
    net/registrar_server.cpp
    net/registrar_client.cpp
    net/static_discovery.cpp
    net/actor_location_cache.cpp
    net/gossip_membership.cpp
    net/udp_transport.cpp
    net/hybrid_discovery.cpp
    net/http_parser.cpp
    net/http_gateway.cpp
    net/http_client.cpp
    net/http_connection.cpp
    net/endpoint_outbound_queue.cpp
    net/endpoint_circuit_breaker.cpp
    sched/actor_ready_gate.cpp
    sched/actor_execution_engine.cpp
    sched/work_placement_scheduler.cpp
    sched/scheduler.cpp
    sched/worker_thread.cpp
    sched/edf_queue.cpp
    sched/a2ws.cpp
    timer/timing_wheel.cpp
    timer/calendar_queue.cpp
    coroutine/coroutine_frame_pool.cpp
    sched/dedicated_thread_pool.cpp
    mem/segment_provider.cpp
    mem/slab_cache.cpp
    mem/thread_local_allocator.cpp
    mem/memory_config.cpp
    mem/memory_region.cpp
    mem/memory_telemetry.cpp
    mem/memory_tracker.cpp
    mem/hibernation_manager.cpp
    mem/guard_page.cpp
    mem/compaction.cpp
    mem/zram.cpp
    fault/fault_point_registry.cpp
    fault/fault_schedule.cpp
    fault/fault_controller.cpp
    fault/fault_points.cpp
    rpc/rpc_channel.cpp
    metrics/metrics_registry.cpp
    metrics/metrics_aggregator.cpp
    metrics/metrics_formatter.cpp
    metrics/metrics_actor.cpp
    log/log_level.cpp
    log/log_category.cpp
    log/log_formatter.cpp
    log/logger.cpp
    log/log_drain.cpp
    log/log_manager.cpp
    log/stderr_sink.cpp
    log/file_sink.cpp
    log/rotating_file_sink.cpp
    log/syslog_sink.cpp
    adt/stream_buffer.cpp
    ../third_party/llhttp/llhttp.c
    ../third_party/llhttp/api.c
    ../third_party/llhttp/http.c
        process/process_manager.cpp
        process/watchdog_actor.cpp
        process/health_http_server.cpp
)

target_include_directories(hpactor_lib
    PUBLIC
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_BINARY_DIR}
        ${CMAKE_SOURCE_DIR}/third_party/llhttp
)

# ---- vendored C sources (llhttp) -------------------------------------------

if(CMAKE_C_COMPILER_ID MATCHES "Clang|AppleClang")
    set(LLHTTP_CLANG_FLAGS "-Wno-implicit-int-conversion")
else()
    set(LLHTTP_CLANG_FLAGS "")
endif()

set_source_files_properties(
    ../third_party/llhttp/llhttp.c
    ../third_party/llhttp/api.c
    ../third_party/llhttp/http.c
    PROPERTIES
        LANGUAGE C
        COMPILE_FLAGS "-Wno-error -Wno-conversion -Wno-sign-conversion -Wno-unused-parameter -Wno-sign-compare -Wno-unused-variable ${LLHTTP_CLANG_FLAGS}"
)

# ---- exceptions for toml++ TUs ---------------------------------------------

set_source_files_properties(
    config/toml_parser.cpp
    config/toml_table_view.cpp
    PROPERTIES COMPILE_FLAGS "-fexceptions"
)

# ---- CLI subsystem (runtime opt-in via cli.enabled) ------------------------

target_sources(hpactor_lib PRIVATE
    cli/command_registry.cpp
    cli/lexer.cpp
    cli/command_node.cpp
    cli/command_tree_builder.cpp
    cli/line_editor.cpp
    cli/pretty_formatter.cpp
    cli/json_formatter.cpp
    cli/tabular_formatter.cpp
    cli/pager.cpp
    cli/interactive_cli_actor.cpp
    cli/cli_connector.cpp
    cli/cli_local_actor.cpp
    cli/cli_session.cpp
    cli/cli_legacy_server_actor.cpp
    cli/local_server_cli_host.cpp
    cli/http_handler.cpp
    cli/cli_http_server_actor.cpp
    cli/cli_client_actor.cpp
    cli/cli_proto_server_actor.cpp
    cli/commands/help_command.cpp
    cli/commands/quit_command.cpp
    cli/commands/actor_commands.cpp
    cli/commands/command_utils.cpp
    cli/commands/system_commands.cpp
    cli/commands/misc_commands.cpp
    cli/commands/failure_commands.cpp
    cli/commands/fault_commands.cpp
    cli/commands/dlq_commands.cpp
    cli/commands/ask_commands.cpp
    cli/commands/client_commands.cpp
    cli/commands/endpoint_commands.cpp
    cli/commands/reliable_commands.cpp
    cli/commands/tracing_commands.cpp
    cli/commands/log_commands.cpp
    cli/commands/scheduler_commands.cpp
    cli/handlers/cli_http_handler_helpers.cpp
    cli/handlers/cli_http_legacy_handler.cpp
    cli/handlers/cli_http_actor_handlers.cpp
    cli/handlers/cli_http_system_handlers.cpp
    cli/handlers/cli_http_fault_handlers.cpp
    cli/handlers/cli_http_dlq_handlers.cpp
    cli/handlers/cli_http_ask_handlers.cpp
)

# ---- AI accelerator subsystem (compile-time gate, runtime opt-in) -----------

if(ENABLE_AI_ACCELERATORS)
    target_sources(hpactor_lib PRIVATE
        ai/accelerator_types.cpp
        ai/ai_message_registry.cpp
        config/parsers/ai_accelerator_config_parser.cpp
    )
endif()

# ---- tracing subsystem (runtime opt-in via tracing.enabled) ----------------

target_sources(hpactor_lib PRIVATE
    tracing/trace_context_parser.cpp
    tracing/sampler.cpp
    tracing/memory_exporter.cpp
    tracing/json_exporter.cpp
    tracing/otlp_exporter.cpp
    tracing/trace_manager.cpp
)

# ---- platform-specific I/O backends ----------------------------------------

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    if(HPACTOR_ENABLE_PROACTOR)
        message(STATUS "Using io_uring (Proactor)")
        target_sources(hpactor_lib PRIVATE net/proactor/iouring_backend.cpp)
    else()
        message(STATUS "Using epoll (Reactor)")
    endif()
    target_sources(hpactor_lib PRIVATE net/reactor/epoll_backend.cpp)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    if(HPACTOR_ENABLE_PROACTOR)
        message(STATUS "Using GCD (Proactor)")
        target_sources(hpactor_lib PRIVATE net/proactor/gcd_backend.cpp)
    else()
        message(STATUS "Using kqueue (Reactor)")
    endif()
    target_sources(hpactor_lib PRIVATE net/reactor/kqueue_backend.cpp)
endif()

# ---- link dependencies -----------------------------------------------------

target_link_libraries(hpactor_lib
    PUBLIC
        tomlplusplus::tomlplusplus
        OpenSSL::SSL
        OpenSSL::Crypto
        protobuf::libprotobuf
        ${HPACTOR_ABSL_LIBS}
        hpactor_proto
)

if(ENABLE_CLI)
    target_link_libraries(hpactor_lib PRIVATE linenoise)
endif()

# ARM64 / RISC-V may need -latomic for std::atomic<uint64_t>
if(UNIX AND NOT APPLE)
    target_link_libraries(hpactor_lib PUBLIC atomic)
endif()

# ---- alias -----------------------------------------------------------------

add_library(hpactor ALIAS hpactor_lib)
