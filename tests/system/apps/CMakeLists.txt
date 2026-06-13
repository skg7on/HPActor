add_executable(test_system_apps
    test_edgeops_messages.cpp
    test_order_platform_messages.cpp
)
target_include_directories(test_system_apps PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(test_system_apps hpactor hpactor_test_support GTest::gtest_main)
gtest_discover_tests(test_system_apps PROPERTIES TIMEOUT 15)

# bench_perf smoke test — spawns actors, runs short benchmark, verifies samples
add_executable(test_bench_perf_smoke test_bench_perf_smoke.cpp)
target_include_directories(test_bench_perf_smoke PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(test_bench_perf_smoke hpactor hpactor_test_support GTest::gtest_main)
gtest_discover_tests(test_bench_perf_smoke PROPERTIES TIMEOUT 30)
