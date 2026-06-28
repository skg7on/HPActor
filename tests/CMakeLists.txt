# Tests — three-tier: unit, integration, system
add_compile_options(-UNDEBUG)

# Shared test support headers (scheduler_test_driver.hpp)
add_library(hpactor_test_support INTERFACE)
target_include_directories(hpactor_test_support INTERFACE
    ${CMAKE_SOURCE_DIR}/tests/support)

# Vendored Google Test
include(${CMAKE_SOURCE_DIR}/cmake/gtest.cmake)

add_subdirectory(architecture)
add_subdirectory(unit)
add_subdirectory(integration)
add_subdirectory(system)
