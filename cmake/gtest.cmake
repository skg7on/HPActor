# Google Test — vendored in third_party/googletest
# Exposes: GTest::gtest (library), GTest::gtest_main (library + main())
#
# Add to a test CMakeLists.txt with:
#   target_link_libraries(my_test GTest::gtest_main)

set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
set(gtest_disable_pthreads OFF CACHE BOOL "" FORCE)

add_subdirectory(
    ${CMAKE_SOURCE_DIR}/third_party/googletest
    ${CMAKE_BINARY_DIR}/googletest
    EXCLUDE_FROM_ALL
)
