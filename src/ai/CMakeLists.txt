# =============================================================================
# src/ai/CMakeLists.txt — AI accelerator subsystem sources
# =============================================================================

target_sources(hpactor_lib PRIVATE
    accelerator_types.cpp
    ai_message_registry.cpp
    ../config/parsers/ai_accelerator_config_parser.cpp
)
