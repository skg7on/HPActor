cmake_minimum_required(VERSION 3.16)

# Generate compile_commands.json for tooling (must be before project())
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# =============================================================================
# Platform-Specific Settings (must be before find_package)
# =============================================================================
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    # Protobuf dylib (libprotobuf.34+) is built for macOS 26.0+
    # CMAKE_OSX_DEPLOYMENT_TARGET must be >= 26.0 to avoid linker warnings
    set(CMAKE_OSX_DEPLOYMENT_TARGET "26.0" CACHE STRING "macOS deployment target")
endif()

project(HPActor VERSION 0.1.0 LANGUAGES C CXX)

# Note: With Ninja, compile_commands.json is placed in CMAKE_SOURCE_DIR by default

# =============================================================================
# Build Options
# =============================================================================
option(ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(ENABLE_TSAN "Enable ThreadSanitizer" OFF)
option(ENABLE_EXAMPLES "Build example programs" ON)
option(ENABLE_PROACTOR "Use Proactor I/O model (io_uring/GCD)" ON)
option(ENABLE_MEMORY_TRACKING "Enable per-actor memory tracking and telemetry" ON)
option(ENABLE_ACTOR_METRICS "Enable actor-level metrics subsystem" ON)
option(ENABLE_MEMORY_DEBUG "Enable memory poisoning, canaries, and guard pages" OFF)
option(ENABLE_CLI "Enable interactive CLI subsystem" ON)
option(ENABLE_ACTOR_LOGGING "Enable structured actor logging subsystem" ON)
option(ENABLE_ACTOR_TRACING "Enable distributed tracing subsystem" ON)
set(HPACTOR_ENABLE_ACTOR_LOGGING ${ENABLE_ACTOR_LOGGING})

# Sanitizer mutual exclusivity
if(ENABLE_ASAN AND ENABLE_TSAN)
    message(FATAL_ERROR "Cannot enable both ASan and TSan simultaneously")
endif()

# =============================================================================
# Compiler Settings - C++20/C++17 with LLVM Standards
# =============================================================================

# User-facing option to override C++20 coroutine auto-detection.
# AUTO (default) — use compiler detection. ON/OFF — force enable/disable.
# When enabled, coroutine scheduling code is compiled in as an ADDITIONAL feature
# alongside the default behavior-based scheduler. The runtime choice between
# coroutine and behavior-based scheduling is controlled via Config::use_coroutines.
set(HPACTOR_ENABLE_COROUTINES "AUTO" CACHE STRING
    "Enable C++20 coroutines (AUTO, ON, OFF)")

# Detect C++20 coroutine support. If the compiler supports C++20 and provides
# the <coroutine> header, coroutine support code is compiled alongside the
# default behavior-based path. Otherwise only behavior-based scheduling is available.
include(CheckCXXSourceCompiles)
include(CheckIncludeFileCXX)

# Check for <coroutine> header availability
check_include_file_cxx(coroutine HPACTOR_HAVE_COROUTINE_HEADER)

# Try to compile a minimal C++20 coroutine program with proper traits
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/coro_test.cpp" "
#include <coroutine>
struct CoroutineTask;
struct CoroutinePromise {
    using handle_type = std::coroutine_handle<CoroutinePromise>;
    std::suspend_always initial_suspend() noexcept { return {}; }
    std::suspend_always final_suspend() noexcept { return {}; }
    void return_void() noexcept {}
    void unhandled_exception() noexcept {}
    CoroutineTask get_return_object();
};
struct CoroutineTask {
    using handle_type = std::coroutine_handle<CoroutinePromise>;
    CoroutineTask() noexcept : h_(nullptr) {}
    explicit CoroutineTask(handle_type h) noexcept : h_(h) {}
    handle_type h() const noexcept { return h_; }
    explicit operator bool() const noexcept { return h_ != nullptr; }
    bool done() const { return !h_ || h_.done(); }
    void resume() { if (h_ && !h_.done()) h_.resume(); }
    ~CoroutineTask() { if (h_) h_.destroy(); }
private:
    handle_type h_;
};
inline CoroutineTask CoroutinePromise::get_return_object() { return CoroutineTask{handle_type::from_promise(*this)}; }
template<> struct std::coroutine_traits<CoroutineTask> { using promise_type = CoroutinePromise; };
CoroutineTask g() { co_return; }
int main() { return 0; }
")

try_compile(HPACTOR_HAVE_CPP20_COROUTINES
    ${CMAKE_CURRENT_BINARY_DIR}/coro_build
    ${CMAKE_CURRENT_BINARY_DIR}/coro_test.cpp
    COMPILE_DEFINITIONS "-std=c++20"
    OUTPUT_VARIABLE HPACTOR_CORO_OUTPUT
)
file(REMOVE "${CMAKE_CURRENT_BINARY_DIR}/coro_test.cpp")

if(HPACTOR_ENABLE_COROUTINES STREQUAL "AUTO")
    if(HPACTOR_HAVE_CPP20_COROUTINES)
        message(STATUS "C++20 coroutines detected - coroutine support compiled in (default: behavior-based)")
        set(CMAKE_CXX_STANDARD 20)
        set(HPACTOR_SUPPORT_COROUTINES ON CACHE BOOL "Compile in C++20 coroutine support" FORCE)
    else()
        message(STATUS "C++20 mode - no coroutine support detected (behavior-based only)")
        set(CMAKE_CXX_STANDARD 20)
        set(HPACTOR_SUPPORT_COROUTINES OFF CACHE BOOL "Compile in C++20 coroutine support" FORCE)
    endif()
elseif(HPACTOR_ENABLE_COROUTINES)
    message(STATUS "C++20 coroutines explicitly enabled - coroutine support compiled in")
    set(CMAKE_CXX_STANDARD 20)
    set(HPACTOR_SUPPORT_COROUTINES ON CACHE BOOL "Compile in C++20 coroutine support" FORCE)
else()
    message(STATUS "C++20 mode - coroutines explicitly disabled (behavior-based only)")
    set(CMAKE_CXX_STANDARD 20)
    set(HPACTOR_SUPPORT_COROUTINES OFF CACHE BOOL "Compile in C++20 coroutine support" FORCE)
endif()

find_program(CLANG_TIDY_EXE NAMES "clang-tidy")
if(CLANG_TIDY_EXE)
    execute_process(COMMAND ${CLANG_TIDY_EXE} --version
        OUTPUT_VARIABLE CLANG_TIDY_VERSION_OUTPUT
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    string(REGEX MATCH "version ([0-9]+)\\.[0-9]+" _ ${CLANG_TIDY_VERSION_OUTPUT})
    if(CMAKE_MATCH_1 AND CMAKE_MATCH_1 GREATER_EQUAL 20)
        message(STATUS "clang-tidy ${CMAKE_MATCH_1}.x found — enabling")
        set(CMAKE_CXX_CLANG_TIDY "${CLANG_TIDY_EXE};-config-file=${CMAKE_SOURCE_DIR}/.clang-tidy")
    else()
        message(STATUS "clang-tidy version ${CMAKE_MATCH_1} (< 20) — skipping")
    endif()
endif()

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
    $<$<COMPILE_LANGUAGE:CXX>:-Wold-style-cast>

)

# Suppress linker warnings for dylibs built for newer macOS versions
# (homebrew protobuf/abseil/openssl are built for macOS 26.0+)
# GNU ld does not support -w; Apple ld does.
if(APPLE)
  add_link_options(-Wl,-w)
endif()

# Suppress abseil logging dependency in protobuf
add_compile_definitions(GOOGLE_PROTOBUF_SUPPRESS_ABSL_LOG)

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
add_library(hpactor_lib SHARED
    src/actor/abstract_actor.cpp
    src/actor/actor_context.cpp
    src/actor/actor_system.cpp
    src/actor/daemon_actor.cpp
    src/actor/event_based_actor.cpp
    src/actor/local_actor.cpp

    src/actor/spawn_receiver.cpp
    src/actor/http_gateway_actor.cpp
    src/actor/lifecycle_actor.cpp
    src/actor/blocking_actor.cpp
    src/actor/scoped_actor.cpp
    src/mailbox/dead_letter_queue.cpp
    src/actor_type_registry.cpp
    src/ref/actor_proxy.cpp
    src/ref/actor_ref.cpp
    src/spawn.cpp
    src/supervision/supervision.cpp
    src/config/actor_factory_registry.cpp
    src/config/toml_parser.cpp
    src/config/toml_table_view.cpp
    src/config/toml_parser_registry.cpp
    src/config/parsers/system_core_config_parser.cpp
    src/config/parsers/metrics_config_parser.cpp
    src/config/parsers/logging_config_parser.cpp
    src/config/parsers/cli_config_parser.cpp
    src/config/parsers/discovery_config_parser.cpp
    src/config/parsers/topology_config_parser.cpp
    src/config/parsers/mailbox_config_parser.cpp
    src/config/parsers/dead_letters_config_parser.cpp
    src/config/parsers/tracing_config_parser.cpp
    src/config/parsers/shutdown_config_parser.cpp
    src/config/binary_serializer.cpp
    src/config/binary_loader.cpp
    src/core/proto_type_registry.cpp
    src/types/typed_message.cpp
    src/net/event_loop.cpp
    src/net/acceptor.cpp
    src/net/connection.cpp
    src/net/tcp_transport.cpp
    src/net/frame.cpp
    src/net/frame_protobuf.cpp
    src/net/tls_context.cpp
    src/net/tls_connection.cpp
    src/net/wireframe_connection.cpp
    src/net/connection_pool.cpp
    src/net/endpoint.cpp
    src/net/registrar.cpp
    src/net/registrar_server.cpp
    src/net/registrar_client.cpp
    src/net/static_discovery.cpp
    src/net/actor_location_cache.cpp
    src/net/gossip_membership.cpp
    src/net/hybrid_discovery.cpp
    src/net/http_parser.cpp
    src/net/http_gateway.cpp
    src/net/http_client.cpp
    src/net/http_connection.cpp
    src/sched/scheduler.cpp
    src/sched/worker_thread.cpp
    src/mem/segment_provider.cpp
    src/mem/slab_cache.cpp
    src/mem/thread_local_allocator.cpp
    src/mem/memory_config.cpp
    src/mem/memory_tracker.cpp
    src/mem/hibernation_manager.cpp
    src/mem/guard_page.cpp
    src/mem/compaction.cpp
    src/mem/zram.cpp
    src/sched/edf_queue.cpp
    src/sched/a2ws.cpp
    src/sched/timing_wheel.cpp
    src/sched/calendar_queue.cpp
    src/sched/coroutine_frame_pool.cpp
    src/sched/dedicated_thread_pool.cpp
    src/rpc/rpc_channel.cpp
    src/metrics/metrics_registry.cpp
    src/metrics/metrics_aggregator.cpp
    src/metrics/metrics_formatter.cpp
    src/metrics/metrics_actor.cpp
    src/log/log_level.cpp
    src/log/log_category.cpp
    src/log/log_formatter.cpp
    src/log/logger.cpp
    src/log/log_drain.cpp
    src/log/log_manager.cpp
    src/log/stderr_sink.cpp
    src/log/file_sink.cpp
    src/log/rotating_file_sink.cpp
    src/adt/stream_buffer.cpp
    third_party/llhttp/llhttp.c
    third_party/llhttp/api.c
    third_party/llhttp/http.c
)
# llhttp sources are C, not C++ — compile with C compiler and relaxed warnings
set_source_files_properties(
    third_party/llhttp/llhttp.c
    third_party/llhttp/api.c
    third_party/llhttp/http.c
    PROPERTIES
        LANGUAGE C
        COMPILE_FLAGS "-Wno-error -Wno-conversion -Wno-sign-conversion -Wno-implicit-int-conversion -Wno-unused-parameter -Wno-sign-compare"
)
target_include_directories(hpactor_lib PUBLIC include ${CMAKE_CURRENT_BINARY_DIR} third_party/llhttp)

target_sources(hpactor_lib PRIVATE
    src/cli/lexer.cpp
    src/cli/command_node.cpp
    src/cli/line_editor.cpp
    src/cli/pretty_formatter.cpp
    src/cli/json_formatter.cpp
    src/cli/tabular_formatter.cpp
    src/cli/pager.cpp
    src/cli/cli_actor.cpp
)
set(HPACTOR_ENABLE_CLI 1)

# Tracing subsystem
if(ENABLE_ACTOR_TRACING)
    set(HPACTOR_ENABLE_ACTOR_TRACING 1)
    target_sources(hpactor_lib PRIVATE
        src/tracing/trace_context_parser.cpp
        src/tracing/sampler.cpp
        src/tracing/memory_exporter.cpp
        src/tracing/json_exporter.cpp
        src/tracing/otlp_exporter.cpp
        src/tracing/trace_manager.cpp
    )
else()
    set(HPACTOR_ENABLE_ACTOR_TRACING 0)
endif()

# Enable exceptions for TOML adapter sources (toml++ requires them)
set(HPACTOR_TOML_EXCEPTION_SOURCES
    src/config/toml_parser.cpp
    src/config/toml_table_view.cpp
)
set_source_files_properties(
    ${HPACTOR_TOML_EXCEPTION_SOURCES}
    PROPERTIES COMPILE_FLAGS "-fexceptions"
)

# Protobuf and Abseil detection
# Protobuf >= 22.0 requires Abseil as a transitive dependency
find_package(Protobuf REQUIRED)

# Detect Protobuf version from protoc (more robust than Protobuf_VERSION)
execute_process(COMMAND ${Protobuf_PROTOC_EXECUTABLE} --version
    OUTPUT_VARIABLE HPACTOR_PROTOC_VERSION_OUTPUT
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
string(REGEX MATCH "[0-9]+\\.[0-9]+" HPACTOR_PROTOC_VERSION "${HPACTOR_PROTOC_VERSION_OUTPUT}")
if(HPACTOR_PROTOC_VERSION VERSION_GREATER_EQUAL "22.0")
    find_package(absl REQUIRED)
    set(HPACTOR_PROTO_ABSL_LIBS absl::log_internal_check_op)
    set(HPACTOR_ABSL_LIBS absl::log absl::log_internal_check_op)
else()
    set(HPACTOR_PROTO_ABSL_LIBS "")
    set(HPACTOR_ABSL_LIBS "")
endif()

# linenoise — vendored line editing library for CLI subsystem
add_library(linenoise STATIC
    third_party/linenoise/linenoise.c
)
set_target_properties(linenoise PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(linenoise PUBLIC third_party/linenoise)
target_compile_options(linenoise PRIVATE
    -Wno-error
    -Wno-conversion
    -Wno-sign-conversion
    -Wno-sign-compare
    -Wno-unused-parameter
    # Clang-specific suppressions (GCC does not recognize these)
    $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-implicit-int-conversion>
    $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-shorten-64-to-32>
    $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-extra-semi-stmt>
)

# Generate protobuf C++ headers/sources
include(${CMAKE_SOURCE_DIR}/cmake/tomlplusplus.cmake)
include(${CMAKE_SOURCE_DIR}/cmake/protobuf.cmake)
PROTOBUF_GENERATE_CPP(PROTO_SRCS PROTO_HDRS
    ${CMAKE_SOURCE_DIR}/protos/hpactor/frame.proto
    ${CMAKE_SOURCE_DIR}/protos/hpactor/common.proto
    ${CMAKE_SOURCE_DIR}/protos/hpactor/messages.proto
    ${CMAKE_SOURCE_DIR}/protos/hpactor/registrar.proto
    ${CMAKE_SOURCE_DIR}/protos/hpactor/gossip.proto
    ${CMAKE_SOURCE_DIR}/protos/hpactor/cli_messages.proto
)
add_library(hpactor_proto SHARED ${PROTO_SRCS} ${PROTO_HDRS})
set_target_properties(hpactor_proto PROPERTIES CXX_CLANG_TIDY "")
target_link_libraries(hpactor_proto PUBLIC protobuf::libprotobuf ${HPACTOR_PROTO_ABSL_LIBS})
target_compile_options(hpactor_proto PRIVATE -Wno-sign-conversion)
target_include_directories(hpactor_proto PUBLIC ${CMAKE_CURRENT_BINARY_DIR})

find_package(OpenSSL REQUIRED)
target_link_libraries(hpactor_lib PUBLIC tomlplusplus::tomlplusplus OpenSSL::SSL OpenSSL::Crypto protobuf::libprotobuf ${HPACTOR_ABSL_LIBS} hpactor_proto)

if(ENABLE_CLI)
    target_link_libraries(hpactor_lib PRIVATE linenoise)
endif()

# ARM64 and some RISC-V platforms require explicit -latomic for std::atomic
# operations (e.g., std::atomic<uint64_t> in CalendarQueue).
# On x86_64 and macOS this is a no-op.
if(UNIX AND NOT APPLE)
  target_link_libraries(hpactor_lib PUBLIC atomic)
endif()

if(ENABLE_PROACTOR)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        if(liburing_FOUND)
            message(STATUS "Using io_uring (Proactor)")
            target_sources(hpactor_lib PRIVATE src/net/proactor/iouring_backend.cpp)
            set(HPACTOR_ENABLE_PROACTOR ON)
        else()
            message(STATUS "liburing not found — falling back to epoll (Reactor)")
            set(HPACTOR_ENABLE_PROACTOR OFF)
        endif()
        # Always include epoll as fallback
        target_sources(hpactor_lib PRIVATE src/net/reactor/epoll_backend.cpp)
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        message(STATUS "Using GCD (Proactor)")
        target_sources(hpactor_lib PRIVATE src/net/proactor/gcd_backend.cpp)
        # Always include kqueue as fallback
        target_sources(hpactor_lib PRIVATE src/net/reactor/kqueue_backend.cpp)
        set(HPACTOR_ENABLE_PROACTOR ON)
    else()
        set(HPACTOR_ENABLE_PROACTOR OFF)
    endif()
else()
    set(HPACTOR_ENABLE_PROACTOR OFF)
    # Reactor mode
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        message(STATUS "Using epoll (Reactor)")
        target_sources(hpactor_lib PRIVATE src/net/reactor/epoll_backend.cpp)
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        message(STATUS "Using kqueue (Reactor)")
        target_sources(hpactor_lib PRIVATE src/net/reactor/kqueue_backend.cpp)
    endif()
endif()

# Generate hpactor_config.hpp from CMake template
configure_file(
    ${CMAKE_SOURCE_DIR}/include/hpactor/hpactor_config.hpp.in
    ${CMAKE_CURRENT_BINARY_DIR}/hpactor/hpactor_config.hpp
    @ONLY
)

# Alias for backwards compatibility
add_library(hpactor ALIAS hpactor_lib)

# =============================================================================
# Tests
# =============================================================================
# AOT Compiler
add_subdirectory(tools/toml-compiler)

enable_testing()
add_subdirectory(tests)

# =============================================================================
# Examples
# =============================================================================
if(ENABLE_EXAMPLES)
    add_subdirectory(examples)
endif()