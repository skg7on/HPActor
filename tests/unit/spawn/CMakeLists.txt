add_executable(test_unit_spawn
    test_spawn_serialization.cpp
    test_spawn_unit.cpp
)
target_link_libraries(test_unit_spawn hpactor hpactor_proto pthread GTest::gtest_main)
gtest_discover_tests(test_unit_spawn)
