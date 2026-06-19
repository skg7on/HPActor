add_executable(test_integration_supervision
    test_supervision.cpp
    test_supervisor_actor.cpp
    test_self_supervising_actor.cpp
    test_self_supervising_remote.cpp
    test_supervision_final.cpp
)
target_link_libraries(test_integration_supervision hpactor hpactor_test_support GTest::gtest_main)
gtest_discover_tests(test_integration_supervision)
