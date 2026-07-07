add_executable(test_unit_python_binding
    test_native_notifier.cpp
    test_python_contracts.cpp
    test_python_runtime_queues.cpp
    test_python_runtime.cpp)
target_link_libraries(test_unit_python_binding PRIVATE
    hpactor_python_native hpactor_test_support GTest::gtest_main)
gtest_discover_tests(test_unit_python_binding)
