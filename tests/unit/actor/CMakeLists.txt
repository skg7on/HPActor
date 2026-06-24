add_executable(test_unit_actor
    test_abstract_actor.cpp
    test_behavior_builder.cpp
    test_behavior_combinators.cpp
    test_behavior_fsm.cpp
    test_circuit_breaker_tracker.cpp
    test_circuit_breaker_result.cpp
    test_failure_rate_tracker.cpp
    test_lifecycle_state.cpp
    test_passivation_lifecycle.cpp
    test_passivation_config.cpp
    test_durable_state_store.cpp
    test_actor_route.cpp
    test_actor_directory.cpp
    test_quarantine_reason.cpp
    routing/test_routing_logic.cpp
    routing/test_pool_router.cpp
    routing/test_group_router.cpp
    receptionist/test_receptionist.cpp
    behavior_test_kit_test.cpp
    test_probe_test.cpp
    test_behavior_message_adapter.cpp
    test_pipe_to.cpp
)
target_link_libraries(test_unit_actor hpactor hpactor_proto pthread hpactor_test_support GTest::gtest_main)
gtest_discover_tests(test_unit_actor)

add_subdirectory(durable)
