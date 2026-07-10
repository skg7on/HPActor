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
    test_python_runtime_stress.cpp)
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
