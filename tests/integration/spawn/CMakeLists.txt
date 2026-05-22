add_executable(test_integration_spawn
    test_actor_type_registry.cpp
    test_async_actor.cpp
    test_spawn_integration.cpp
    test_spawn_receiver.cpp
)
target_link_libraries(test_integration_spawn
    hpactor hpactor_proto pthread hpactor_test_support GTest::gtest_main
)
gtest_discover_tests(test_integration_spawn)
