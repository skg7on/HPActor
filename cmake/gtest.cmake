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

include(GoogleTest)
