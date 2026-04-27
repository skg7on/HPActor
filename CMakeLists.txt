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
# Compiler Settings - C++20/C++17 with LLVM Standards
# =============================================================================

# Detect C++20 coroutine support. If the compiler supports C++20 and provides
# the <coroutine> header, use C++20 features. Otherwise fall back to C++17.
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

if(HPACTOR_HAVE_CPP20_COROUTINES)
    message(STATUS "C++20 coroutines detected - using coroutine-based scheduling")
    set(CMAKE_CXX_STANDARD 20)
    set(HPACTOR_USE_COROUTINES ON CACHE BOOL "Enable C++20 coroutines" FORCE)
    add_compile_definitions(HPACTOR_USE_COROUTINES=1)
else()
    message(STATUS "C++17 mode - using callback-based scheduling")
    set(CMAKE_CXX_STANDARD 17)
    set(HPACTOR_USE_COROUTINES OFF CACHE BOOL "Enable C++20 coroutines" FORCE)
    add_compile_definitions(HPACTOR_USE_COROUTINES=0)
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
    -Wold-style-cast
)

# Suppress linker warnings for dylibs built for newer macOS versions
# (homebrew protobuf/abseil/openssl are built for macOS 26.0+)
add_link_options(-Wl,-w)

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
# Platform-Specific Settings
# =============================================================================
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    # Protobuf dylib (libprotobuf.34+) is built for macOS 26.0+
    # CMAKE_OSX_DEPLOYMENT_TARGET must be >= 26.0 to avoid linker warnings
    set(CMAKE_OSX_DEPLOYMENT_TARGET "26.0" CACHE STRING "macOS deployment target")
endif()

# =============================================================================
# Library Sources
# =============================================================================
add_library(hpactor_lib SHARED
    src/actor/abstract_actor.cpp
    src/actor/actor_context.cpp
    src/actor/actor_system.cpp
    src/actor/event_based_actor.cpp
    src/actor/local_actor.cpp
    src/actor/scheduler.cpp
    src/actor/spawn_receiver.cpp
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
    src/net/frame_protobuf.cpp
    src/net/tls_context.cpp
    src/net/tls_connection.cpp
    src/net/plain_connection.cpp
    src/net/connection_pool.cpp
    src/net/endpoint.cpp
    src/net/registrar.cpp
    src/net/registrar_server.cpp
    src/net/registrar_client.cpp
    src/sched/scheduler.cpp
    src/sched/worker_thread.cpp
    src/sched/edf_queue.cpp
    src/sched/a2ws.cpp
    src/sched/timing_wheel.cpp
    src/sched/coroutine_frame_pool.cpp
    src/rpc/rpc_channel.cpp
)
target_include_directories(hpactor_lib PUBLIC include)

# Generate protobuf C++ headers/sources
include(${CMAKE_SOURCE_DIR}/cmake/protobuf.cmake)
PROTOBUF_GENERATE_CPP(PROTO_SRCS PROTO_HDRS
    ${CMAKE_SOURCE_DIR}/protos/hpactor/frame.proto
    ${CMAKE_SOURCE_DIR}/protos/hpactor/common.proto
    ${CMAKE_SOURCE_DIR}/protos/hpactor/messages.proto
    ${CMAKE_SOURCE_DIR}/protos/hpactor/registrar.proto
)
add_library(hpactor_proto SHARED ${PROTO_SRCS} ${PROTO_HDRS})
target_link_libraries(hpactor_proto PUBLIC protobuf::libprotobuf absl::log_internal_check_op)
target_include_directories(hpactor_proto PUBLIC ${CMAKE_CURRENT_BINARY_DIR})

find_package(OpenSSL REQUIRED)
find_package(Protobuf REQUIRED)
find_package(absl REQUIRED)
target_link_libraries(hpactor_lib PUBLIC OpenSSL::SSL OpenSSL::Crypto protobuf::libprotobuf absl::log absl::log_internal_check_op hpactor_proto)

# Async I/O backends
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # Try to find liburing, make it optional - fall back to epoll if not found
    find_package(liburing)
    if(liburing_FOUND)
        message(STATUS "liburing found - using io_uring backend")
        target_link_libraries(hpactor_lib PUBLIC liburing::uring)
        target_sources(hpactor_lib PRIVATE src/net/proactor/iouring_backend.cpp src/net/reactor/epoll_backend.cpp)
    else()
        message(STATUS "liburing not found - using epoll backend only (no io_uring support)")
        target_sources(hpactor_lib PRIVATE src/net/reactor/epoll_backend.cpp)
    endif()
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    # Compute macOS SDK path and link libdispatch from the SDK
    execute_process(COMMAND xcrun --show-sdk-path OUTPUT_VARIABLE HPACTOR_SDK_PATH OUTPUT_STRIP_TRAILING_WHITESPACE)
    string(STRIP "${HPACTOR_SDK_PATH}" HPACTOR_SDK_PATH)
    set(HPACTOR_DISPATCH_LIB "${HPACTOR_SDK_PATH}/usr/lib/system/libdispatch.tbd")
    if(NOT EXISTS "${HPACTOR_DISPATCH_LIB}")
        message(FATAL_ERROR "libdispatch not found at ${HPACTOR_DISPATCH_LIB}")
    endif()
    target_link_libraries(hpactor_lib PUBLIC "${HPACTOR_DISPATCH_LIB}")
    # GCD backend (with kqueue fallback)
    target_sources(hpactor_lib PRIVATE src/net/gcd_backend.cpp src/net/reactor/kqueue_backend.cpp)
endif()

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