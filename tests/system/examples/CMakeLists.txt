add_executable(test_system_examples
    test_order_platform_messages.cpp
)
target_include_directories(test_system_examples PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(test_system_examples hpactor hpactor_test_support GTest::gtest_main)
gtest_discover_tests(test_system_examples PROPERTIES TIMEOUT 15)
