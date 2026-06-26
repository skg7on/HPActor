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

add_executable(test_bench_caf_actor_creation test_bench_caf_actor_creation.cpp)
target_include_directories(test_bench_caf_actor_creation PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(test_bench_caf_actor_creation hpactor hpactor_test_support GTest::gtest_main)
gtest_discover_tests(test_bench_caf_actor_creation PROPERTIES TIMEOUT 30)

add_executable(test_bench_caf_mailbox_n1 test_bench_caf_mailbox_n1.cpp)
target_include_directories(test_bench_caf_mailbox_n1 PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(test_bench_caf_mailbox_n1 hpactor hpactor_test_support GTest::gtest_main)
gtest_discover_tests(test_bench_caf_mailbox_n1 PROPERTIES TIMEOUT 30)

add_executable(test_bench_caf_mixed_case test_bench_caf_mixed_case.cpp)
target_include_directories(test_bench_caf_mixed_case PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(test_bench_caf_mixed_case hpactor hpactor_test_support GTest::gtest_main)
gtest_discover_tests(test_bench_caf_mixed_case PROPERTIES TIMEOUT 30)
