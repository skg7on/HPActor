add_executable(test_integration_python_binding
    test_python_native_workflow.cpp
    test_python_topology_transaction.cpp)
target_link_libraries(test_integration_python_binding PRIVATE
    hpactor_python_native hpactor_test_support GTest::gtest_main)
gtest_discover_tests(test_integration_python_binding PROPERTIES TIMEOUT 35)
