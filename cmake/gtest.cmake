# Google Test — vendored in third_party/googletest
# Exposes: GTest::gtest (library), GTest::gtest_main (library + main())
#
# Add to a test CMakeLists.txt with:
#   target_link_libraries(my_test GTest::gtest_main)

set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
set(gtest_disable_pthreads OFF CACHE BOOL "" FORCE)

# Disable clang-tidy for vendored GTest (it uses .cc includes that trigger
# bugprone-suspicious-include, and we don't lint third-party code).
set(_hpactor_saved_clang_tidy "${CMAKE_CXX_CLANG_TIDY}")
set(CMAKE_CXX_CLANG_TIDY "")

add_subdirectory(
    ${CMAKE_SOURCE_DIR}/third_party/googletest
    ${CMAKE_BINARY_DIR}/googletest
    EXCLUDE_FROM_ALL
)

set(CMAKE_CXX_CLANG_TIDY "${_hpactor_saved_clang_tidy}")

# The vendored gtest (v1.14.0) uses SYSTEM INTERFACE for its include dirs,
# producing -isystem paths. On systems with a homebrew-installed gtest
# (v1.17.0+), /opt/homebrew/include (also -isystem from imported dep targets)
# can shadow the vendored headers. v1.17.0 changed MakeAndRegisterTestInfo's
# first parameter from const char* to std::string, causing linker errors.
#
# Fix: demote the vendored gtest include dirs from SYSTEM to regular so they
# produce -I instead of -isystem. -I is always searched before -isystem.
# To suppress -Wsign-compare warnings that now escape from gtest macros under
# -Werror, we add -Wno-sign-compare as an INTERFACE option on the gtest
# targets so it propagates to all test consumers.
get_target_property(_gtest_inc_dirs gtest INTERFACE_INCLUDE_DIRECTORIES)
set_target_properties(gtest PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "")
target_include_directories(gtest BEFORE INTERFACE ${_gtest_inc_dirs})
target_compile_options(gtest INTERFACE -Wno-sign-compare)
get_target_property(_gtest_main_inc_dirs gtest_main INTERFACE_INCLUDE_DIRECTORIES)
set_target_properties(gtest_main PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "")
target_include_directories(gtest_main BEFORE INTERFACE ${_gtest_main_inc_dirs})
target_compile_options(gtest_main INTERFACE -Wno-sign-compare)

include(GoogleTest)
