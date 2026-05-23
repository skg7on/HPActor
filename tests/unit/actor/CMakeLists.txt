add_executable(test_unit_actor
    test_abstract_actor.cpp
    test_lifecycle_state.cpp
    test_proto_registry.cpp
)
target_link_libraries(test_unit_actor hpactor hpactor_proto pthread GTest::gtest_main)
gtest_discover_tests(test_unit_actor)
