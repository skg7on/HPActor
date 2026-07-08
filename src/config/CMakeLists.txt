# =============================================================================
# src/config/CMakeLists.txt — Config subsystem sources
# =============================================================================

target_sources(hpactor_lib PRIVATE
    actor_factory_registry.cpp
    binary_loader.cpp
    binary_serializer.cpp
    python_binding_config.cpp
    toml_parser.cpp
    toml_parser_registry.cpp
    toml_table_view.cpp
    parsers/cli_config_parser.cpp
    parsers/dead_letters_config_parser.cpp
    parsers/delivery_config_parser.cpp
    parsers/discovery_config_parser.cpp
    parsers/logging_config_parser.cpp
    parsers/mailbox_config_parser.cpp
    parsers/metrics_config_parser.cpp
    parsers/passivation_config_parser.cpp
    parsers/process_config_parser.cpp
    parsers/python_binding_config_parser.cpp
    parsers/quarantine_parser.cpp
    parsers/shutdown_config_parser.cpp
    parsers/system_core_config_parser.cpp
    parsers/topology_config_parser.cpp
    parsers/tracing_config_parser.cpp
)
