# FuzzTest.cmake — Build support for libFuzzer-based fuzz targets.
#
# Usage:
#   add_fuzz_test(name SOURCES fuzz_target.cpp LIBRARIES hpactor_lib)
#
# This macro creates an executable linked with -fsanitize=fuzzer,address,undefined.
# Fuzz targets are NOT registered with CTest and are excluded from the default
# "all" build target — build them explicitly by name.

# ---------------------------------------------------------------------------
# Guard: fuzz targets require Clang (libFuzzer).
# ---------------------------------------------------------------------------
if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND NOT CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
    message(WARNING "libFuzzer requires Clang. "
                    "CMAKE_CXX_COMPILER_ID=${CMAKE_CXX_COMPILER_ID}. "
                    "Fuzz targets will not be built. "
                    "Re-run with -DCMAKE_CXX_COMPILER=clang++")
    return()
endif()

# Sanitizer composition.  ASAN is omitted by default because of a known
# macOS ARM conflict with libFuzzer (asan_init_is_running CHECK failure).
# Set -DFUZZ_WITH_ASAN=ON to opt into ASAN on supported platforms.
if(FUZZ_WITH_ASAN)
    set(FUZZ_SANITIZERS "fuzzer,address,undefined")
else()
    set(FUZZ_SANITIZERS "fuzzer,undefined")
endif()
message(STATUS "Fuzz testing enabled (-fsanitize=${FUZZ_SANITIZERS})")

message(STATUS "Fuzz testing enabled (compiler supports -fsanitize=fuzzer,address,undefined)")

# Sanity check: libFuzzer requires Clang >= 6.0
if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND NOT CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
    message(WARNING "libFuzzer requires Clang.  "
                    "CMAKE_CXX_COMPILER_ID=${CMAKE_CXX_COMPILER_ID} may not produce working fuzz targets.")
endif()

# ---------------------------------------------------------------------------
# add_fuzz_test — declare a single fuzz target
# ---------------------------------------------------------------------------
function(add_fuzz_test name)
    set(options "")
    set(one_value_args "")
    set(multi_value_args SOURCES LIBRARIES)
    cmake_parse_arguments(FUZZ "${options}" "${one_value_args}" "${multi_value_args}" ${ARGN})

    set(target_name "fuzz_${name}")
    add_executable(${target_name} EXCLUDE_FROM_ALL ${FUZZ_SOURCES})

    target_compile_options(${target_name} PRIVATE
        -fsanitize=${FUZZ_SANITIZERS}
        -fno-omit-frame-pointer
        -g
    )
    target_link_options(${target_name} PRIVATE
        -fsanitize=${FUZZ_SANITIZERS}
        -fno-omit-frame-pointer
    )
    target_link_libraries(${target_name} PRIVATE ${FUZZ_LIBRARIES})
    target_include_directories(${target_name} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}  # for fuzz_harness.hpp
    )

    # Fuzz targets are NOT part of "all", NOT registered with CTest.
    message(STATUS "  Fuzz target: ${target_name}")
endfunction()
