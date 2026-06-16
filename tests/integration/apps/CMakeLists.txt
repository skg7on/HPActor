add_executable(test_integration_apps
    test_hpactor_demo_foreground.cpp
    ${CMAKE_SOURCE_DIR}/apps/hpactor_demo/cli_demo_actor_factory.cpp
)
target_link_libraries(test_integration_apps hpactor hpactor_test_support GTest::gtest_main)
target_include_directories(test_integration_apps PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/apps/cli_demo
)
gtest_discover_tests(test_integration_apps)
