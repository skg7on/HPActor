# =============================================================================
# compiler_setup.cmake — C++ standard, coroutine detection, warnings, sanitizers
# =============================================================================
#
# Included from the top-level CMakeLists.txt (same scope).  Sets:
#   CMAKE_CXX_STANDARD, HPACTOR_SUPPORT_COROUTINES, compiler flags,
#   sanitizer flags, and clang-tidy integration.

# ---- C++20 coroutine auto-detection ----------------------------------------

set(HPACTOR_ENABLE_COROUTINES "AUTO" CACHE STRING
    "Enable C++20 coroutines (AUTO, ON, OFF)")

include(CheckCXXSourceCompiles)
include(CheckIncludeFileCXX)

check_include_file_cxx(coroutine HPACTOR_HAVE_COROUTINE_HEADER)

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
        message(STATUS "C++20 coroutines detected - coroutine support compiled in")
        set(CMAKE_CXX_STANDARD 20)
        set(HPACTOR_SUPPORT_COROUTINES ON CACHE BOOL "Compile in C++20 coroutine support" FORCE)
    else()
        message(STATUS "C++20 mode - no coroutine support detected (behavior-based only)")
        set(CMAKE_CXX_STANDARD 20)
        set(HPACTOR_SUPPORT_COROUTINES OFF CACHE BOOL "Compile in C++20 coroutine support" FORCE)
    endif()
elseif(HPACTOR_ENABLE_COROUTINES)
    message(STATUS "C++20 coroutines explicitly enabled")
    set(CMAKE_CXX_STANDARD 20)
    set(HPACTOR_SUPPORT_COROUTINES ON CACHE BOOL "Compile in C++20 coroutine support" FORCE)
else()
    message(STATUS "C++20 mode - coroutines explicitly disabled (behavior-based only)")
    set(CMAKE_CXX_STANDARD 20)
    set(HPACTOR_SUPPORT_COROUTINES OFF CACHE BOOL "Compile in C++20 coroutine support" FORCE)
endif()

set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---- clang-tidy ------------------------------------------------------------

if(ENABLE_CLANG_TIDY)
    find_program(CLANG_TIDY_EXE NAMES "clang-tidy")
    if(CLANG_TIDY_EXE)
        execute_process(COMMAND ${CLANG_TIDY_EXE} --version
            OUTPUT_VARIABLE CLANG_TIDY_VERSION_OUTPUT
            OUTPUT_STRIP_TRAILING_WHITESPACE)
        string(REGEX MATCH "version ([0-9]+)\\.[0-9]+" _ "${CLANG_TIDY_VERSION_OUTPUT}")
        if(CMAKE_MATCH_1 AND CMAKE_MATCH_1 GREATER_EQUAL 20)
            message(STATUS "clang-tidy ${CMAKE_MATCH_1}.x found - enabling")
            set(CMAKE_CXX_CLANG_TIDY "${CLANG_TIDY_EXE};-config-file=${CMAKE_SOURCE_DIR}/.clang-tidy")
        else()
            message(STATUS "clang-tidy version ${CMAKE_MATCH_1} (< 20) - skipping")
        endif()
    else()
        message(STATUS "clang-tidy requested but not found - skipping")
    endif()
else()
    message(STATUS "clang-tidy build checks disabled")
endif()

# ---- compiler flags --------------------------------------------------------

add_compile_options(
    -fno-exceptions
    $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
    -Werror
    -Wall
    -Wextra
    -Wshadow
    -Wconversion
    $<$<COMPILE_LANGUAGE:CXX>:-Wold-style-cast>
)

add_compile_definitions(GOOGLE_PROTOBUF_SUPPRESS_ABSL_LOG)

# macOS: suppress linker warnings for dylibs built for newer macOS versions
if(APPLE)
    add_link_options(-Wl,-w)
endif()

# ---- sanitizers ------------------------------------------------------------

if(ENABLE_ASAN AND ENABLE_TSAN)
    message(FATAL_ERROR "Cannot enable both ASan and TSan simultaneously")
endif()

if(ENABLE_ASAN)
    message(STATUS "AddressSanitizer enabled")
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
elseif(ENABLE_TSAN)
    message(STATUS "ThreadSanitizer enabled")
    add_compile_options(-fsanitize=thread)
    add_link_options(-fsanitize=thread)
endif()
