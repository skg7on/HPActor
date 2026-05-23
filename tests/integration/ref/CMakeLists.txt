add_executable(test_integration_ref
    test_actor_ref.cpp
    test_actor_proxy.cpp
    test_actor_proxy_dead_letters.cpp
)
target_link_libraries(test_integration_ref
    hpactor hpactor_test_support GTest::gtest_main
)
gtest_discover_tests(test_integration_ref)
