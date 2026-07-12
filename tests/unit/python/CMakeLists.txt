find_package(Python3 COMPONENTS Development)

add_executable(test_unit_python_binding
    test_native_notifier.cpp
    test_python_binding_config.cpp
    test_python_bridge_actor.cpp
    test_python_command_proto.cpp
    test_python_contracts.cpp
    test_python_runtime_queues.cpp
    test_python_runtime.cpp
    test_python_gateway_actor.cpp
    test_python_runtime_stress.cpp
    test_python_topology_types.cpp
    test_python_topology_preparer.cpp
    test_python_topology_provider.cpp)
target_link_libraries(test_unit_python_binding PRIVATE
    hpactor_python_native hpactor_test_support GTest::gtest_main)
target_include_directories(test_unit_python_binding PRIVATE
    ${CMAKE_SOURCE_DIR}/src)
gtest_discover_tests(test_unit_python_binding)

# Separate binary for pybind11 tests — needs RTTI and exceptions
add_executable(test_pybind11_availability
    test_pybind11_availability.cpp)
target_link_libraries(test_pybind11_availability PRIVATE
    hpactor_python_native hpactor_test_support GTest::gtest_main)
target_include_directories(test_pybind11_availability PRIVATE
    ${CMAKE_SOURCE_DIR}/src)
target_include_directories(test_pybind11_availability SYSTEM PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/pybind11/include
    ${Python3_INCLUDE_DIRS})
target_compile_options(test_pybind11_availability PRIVATE
    -fexceptions)
gtest_discover_tests(test_pybind11_availability)

# ── RTTI-enabled GTest for pybind11 tests ─────────────────────────────
# pybind11 headers need typeid (no way around it).  The main GTest
# library was compiled with -fno-rtti, so we build a tiny RTTI-enabled
# GTest just for the pybind11-specific test binaries.
set(GTEST_RTTI_DIR ${CMAKE_BINARY_DIR}/gtest_rtti)
file(MAKE_DIRECTORY ${GTEST_RTTI_DIR})
add_library(gtest_rtti
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/src/gtest-all.cc)
target_include_directories(gtest_rtti PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include)
target_compile_options(gtest_rtti PRIVATE -fexceptions -frtti)
target_compile_definitions(gtest_rtti PRIVATE GTEST_HAS_RTTI=1)
target_link_libraries(gtest_rtti PRIVATE pthread)

add_library(gtest_main_rtti
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/src/gtest_main.cc)
target_include_directories(gtest_main_rtti PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include)
target_compile_options(gtest_main_rtti PRIVATE -fexceptions -frtti)
target_link_libraries(gtest_main_rtti PRIVATE gtest_rtti)

# Separate binary for NativeSystemObject tests — needs RTTI + exceptions
add_executable(test_pybind11_native_system
    test_pybind11_native_system.cpp
    ${CMAKE_SOURCE_DIR}/bindings/python/native/src/python_pybind11/native_system.cpp)
target_link_libraries(test_pybind11_native_system PRIVATE
    hpactor_python_native hpactor_test_support gtest_main_rtti gtest_rtti Python3::Python)
target_include_directories(test_pybind11_native_system PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/bindings/python/native/include
    ${CMAKE_SOURCE_DIR}/bindings/python/native/src/python_pybind11)
target_include_directories(test_pybind11_native_system SYSTEM PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/pybind11/include
    ${Python3_INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest)
target_compile_options(test_pybind11_native_system PRIVATE
    -fexceptions -frtti)
gtest_discover_tests(test_pybind11_native_system)
