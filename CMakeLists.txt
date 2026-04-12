cmake_minimum_required(VERSION 3.16)

# Generate compile_commands.json for tooling (must be before project())
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

project(HPActor VERSION 0.1.0 LANGUAGES CXX)

# Note: With Ninja, compile_commands.json is placed in CMAKE_SOURCE_DIR by default

# =============================================================================
# Build Options
# =============================================================================
option(ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(ENABLE_TSAN "Enable ThreadSanitizer" OFF)
option(ENABLE_EXAMPLES "Build example programs" ON)

# Sanitizer mutual exclusivity
if(ENABLE_ASAN AND ENABLE_TSAN)
    message(FATAL_ERROR "Cannot enable both ASan and TSan simultaneously")
endif()

# =============================================================================
# Compiler Settings - C++20 with LLVM Standards
# =============================================================================
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# LLVM Coding Standards: no RTTI, no exceptions, stack allocation priority
# Exceptions disabled via -fno-exceptions
# RTTI disabled via -fno-rtti
add_compile_options(
    -fno-exceptions
    -fno-rtti
    -Werror
    -Wall
    -Wextra
    -Wshadow
    -Wconversion
    -Wold-style-cast
)

# =============================================================================
# Sanitizers
# =============================================================================
if(ENABLE_ASAN)
    message(STATUS "AddressSanitizer enabled")
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
elseif(ENABLE_TSAN)
    message(STATUS "ThreadSanitizer enabled")
    add_compile_options(-fsanitize=thread)
    add_link_options(-fsanitize=thread)
endif()

# =============================================================================
# Library Sources
# =============================================================================
add_library(hpactor_lib STATIC
    src/actor/abstract_actor.cpp
    src/actor/actor_context.cpp
    src/actor/actor_system.cpp
    src/actor/event_based_actor.cpp
    src/actor/local_actor.cpp
    src/actor/scheduler.cpp
    src/actor_type_registry.cpp
    src/ref/actor_proxy.cpp
    src/ref/actor_ref.cpp
    src/spawn.cpp
    src/supervision/supervision.cpp
    src/core/serialization.cpp
    src/net/event_loop.cpp
    src/net/acceptor.cpp
    src/net/connection.cpp
    src/net/tcp_transport.cpp
    src/net/frame.cpp
    src/net/tls_context.cpp
    src/net/tls_connection.cpp
    src/net/connection_pool.cpp
    src/net/registrar.cpp
)
target_include_directories(hpactor_lib PUBLIC include)

find_package(OpenSSL REQUIRED)
target_link_libraries(hpactor_lib PUBLIC OpenSSL::SSL OpenSSL::Crypto)

# Alias for backwards compatibility
add_library(hpactor ALIAS hpactor_lib)

# =============================================================================
# Tests
# =============================================================================
enable_testing()
add_subdirectory(tests)

# =============================================================================
# Examples
# =============================================================================
if(ENABLE_EXAMPLES)
    add_subdirectory(examples)
endif()