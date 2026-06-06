add_executable(test_unit_actor
    test_abstract_actor.cpp
    test_circuit_breaker_tracker.cpp
    test_circuit_breaker_result.cpp
    test_failure_rate_tracker.cpp
    test_lifecycle_state.cpp
    test_passivation_lifecycle.cpp
    test_passivation_config.cpp
    test_durable_state_store.cpp
    test_proto_registry.cpp
    test_actor_directory.cpp
    test_quarantine_reason.cpp
)
target_link_libraries(test_unit_actor hpactor hpactor_proto pthread GTest::gtest_main)
gtest_discover_tests(test_unit_actor)
