cmake_minimum_required(VERSION 3.16)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# =============================================================================
# Platform-specific settings (must be before project())
# =============================================================================
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    # Protobuf dylib (libprotobuf.34+) is built for macOS 26.0+
    set(CMAKE_OSX_DEPLOYMENT_TARGET "26.0" CACHE STRING "macOS deployment target")
endif()

project(HPActor VERSION 0.1.0 LANGUAGES C CXX)

# =============================================================================
# Build options
# =============================================================================
option(ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(ENABLE_TSAN "Enable ThreadSanitizer" OFF)
option(ENABLE_EXAMPLES "Build example programs" ON)
option(ENABLE_APPS "Build complex demo applications" ON)
option(ENABLE_PROACTOR "Use Proactor I/O model (io_uring/GCD)" ON)
option(ENABLE_MEMORY_TRACKING "Enable per-actor memory tracking and telemetry" ON)
option(ENABLE_ACTOR_METRICS "Enable actor-level metrics subsystem" ON)
option(ENABLE_MEMORY_DEBUG "Enable memory poisoning, canaries, and guard pages" OFF)
option(ENABLE_CLI "Enable interactive CLI subsystem" ON)
option(ENABLE_ACTOR_LOGGING "Enable structured actor logging subsystem" ON)
option(ENABLE_ACTOR_TRACING "Enable distributed tracing subsystem" ON)
option(ENABLE_COVERAGE "Enable code coverage instrumentation (gcc/clang)" OFF)
option(ENABLE_TESTS "Build tests" ON)
option(ENABLE_RELACY_TESTS "Build relacy-based exhaustive verification tests" OFF)
option(ENABLE_FAULT_INJECTION "Enable deterministic fault injection hooks" ON)
option(ENABLE_ACTOR_PASSIVATION "Enable actor passivation subsystem" ON)
option(ENABLE_CLANG_TIDY "Run clang-tidy during C++ builds when clang-tidy >= 20 is available" OFF)

# =============================================================================
# Compiler setup (C++20, coroutines, warnings, sanitizers, clang-tidy)
# =============================================================================
include(cmake/compiler_setup.cmake)

# =============================================================================
# Coverage instrumentation
# =============================================================================
include(cmake/coverage.cmake)

# =============================================================================
# Dependencies (protobuf, OpenSSL, vendored libs, codegen)
# =============================================================================
include(cmake/dependencies.cmake)

# =============================================================================
# Proactor detection (must precede configure_file)
# =============================================================================
if(ENABLE_PROACTOR)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        if(liburing_FOUND)
            set(HPACTOR_ENABLE_PROACTOR 1)
        else()
            set(HPACTOR_ENABLE_PROACTOR 0)
        endif()
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        set(HPACTOR_ENABLE_PROACTOR 1)
    else()
        set(HPACTOR_ENABLE_PROACTOR 0)
    endif()
else()
    set(HPACTOR_ENABLE_PROACTOR 0)
endif()

# =============================================================================
# Generated configuration header
# =============================================================================
set(HPACTOR_ENABLE_MEMORY_TRACKING ${ENABLE_MEMORY_TRACKING})
set(HPACTOR_ENABLE_MEMORY_DEBUG ${ENABLE_MEMORY_DEBUG})
set(HPACTOR_ENABLE_CLI ${ENABLE_CLI})
set(HPACTOR_ENABLE_ACTOR_LOGGING ${ENABLE_ACTOR_LOGGING})
set(HPACTOR_ENABLE_ACTOR_TRACING ${ENABLE_ACTOR_TRACING})
set(HPACTOR_ENABLE_FAULT_INJECTION ${ENABLE_FAULT_INJECTION})
set(HPACTOR_ENABLE_PASSIVATION ${ENABLE_ACTOR_PASSIVATION})

configure_file(
    ${CMAKE_SOURCE_DIR}/include/hpactor/hpactor_config.hpp.in
    ${CMAKE_CURRENT_BINARY_DIR}/hpactor/hpactor_config.hpp
    @ONLY
)

# =============================================================================
# Subdirectories
# =============================================================================
add_subdirectory(src)

# AOT compiler — depends on hpactor_lib, so must come after src/
add_subdirectory(tools/toml-compiler)

# Standalone CLI client — connects to hpactor daemon via UDS or TCP
add_subdirectory(tools/hpactor-cli)

if(ENABLE_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(ENABLE_EXAMPLES)
    add_subdirectory(examples)
endif()

if(ENABLE_APPS)
    add_subdirectory(apps)
endif()
