# Fuzz tests — coverage-guided fuzz targets for input-facing parsers/decoders.
#
# These targets are only built when ENABLE_FUZZ=ON and require a
# libFuzzer-compatible compiler (Clang >= 6.0).
#
# Build: cmake -DENABLE_FUZZ=ON .. && ninja fuzz_frame fuzz_protobuf ...
# Run:   ./tests/fuzz/fuzz_frame -max_total_time=10

include(${CMAKE_SOURCE_DIR}/cmake/FuzzTest.cmake)

if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND NOT CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
    return()
endif()

# Shared fuzz harness header (header-only, no compile needed)
add_library(fuzz_harness INTERFACE)
target_include_directories(fuzz_harness INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

# ---------------------------------------------------------------------------
# Critical targets (network ingress)
# ---------------------------------------------------------------------------
add_fuzz_test(frame
    SOURCES fuzz_frame.cpp
    LIBRARIES fuzz_harness hpactor_lib
)

add_fuzz_test(protobuf_decode
    SOURCES fuzz_protobuf_decode.cpp
    LIBRARIES fuzz_harness hpactor_lib
)

# ---------------------------------------------------------------------------
# High-value targets (HTTP + TOML)
# ---------------------------------------------------------------------------
add_fuzz_test(http_parser
    SOURCES fuzz_http_parser.cpp
    LIBRARIES fuzz_harness hpactor_lib
)

add_fuzz_test(toml
    SOURCES fuzz_toml.cpp
    LIBRARIES fuzz_harness hpactor_lib
)
# toml++ requires -fexceptions (same exemption as toml_parser.cpp)
target_compile_options(fuzz_toml PRIVATE -fexceptions)

# ---------------------------------------------------------------------------
# Medium targets (CLI + Admin)
# ---------------------------------------------------------------------------
add_fuzz_test(cli_lexer
    SOURCES fuzz_cli_lexer.cpp
    LIBRARIES fuzz_harness hpactor_lib
)

add_fuzz_test(admin_api
    SOURCES fuzz_admin_api.cpp
    LIBRARIES fuzz_harness hpactor_lib
)
