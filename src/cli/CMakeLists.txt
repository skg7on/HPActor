# =============================================================================
# src/cli/CMakeLists.txt — CLI subsystem sources (runtime opt-in via cli.enabled)
# =============================================================================

target_sources(hpactor_lib PRIVATE
    admin/admin_api_actor.cpp
    actor/cli_client_actor.cpp
    actor/cli_http_server_actor.cpp
    actor/cli_legacy_server_actor.cpp
    actor/cli_local_actor.cpp
    actor/cli_proto_server_actor.cpp
    actor/interactive_cli_actor.cpp
    command/cli_session.cpp
    command/command_node.cpp
    command/command_registry.cpp
    command/command_tree_builder.cpp
    commands/actor_commands.cpp
    commands/ask_commands.cpp
    commands/client_commands.cpp
    commands/command_utils.cpp
    commands/dlq_commands.cpp
    commands/endpoint_commands.cpp
    commands/failure_commands.cpp
    commands/fault_commands.cpp
    commands/help_command.cpp
    commands/log_commands.cpp
    commands/misc_commands.cpp
    commands/quit_command.cpp
    commands/reliable_commands.cpp
    commands/scheduler_commands.cpp
    commands/system_commands.cpp
    commands/tracing_commands.cpp
    format/json_formatter.cpp
    format/pretty_formatter.cpp
    format/tabular_formatter.cpp
    handlers/cli_http_actor_handlers.cpp
    handlers/cli_http_ask_handlers.cpp
    handlers/cli_http_dlq_handlers.cpp
    handlers/cli_http_fault_handlers.cpp
    handlers/cli_http_handler_helpers.cpp
    handlers/cli_http_legacy_handler.cpp
    handlers/cli_http_system_handlers.cpp
    host/local_server_cli_host.cpp
    http_handler.cpp
    io/cli_connector.cpp
    io/lexer.cpp
    io/line_editor.cpp
    io/pager.cpp
)
