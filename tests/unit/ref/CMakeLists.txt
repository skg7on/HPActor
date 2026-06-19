add_executable(test_unit_ref
    test_actor_address.cpp
    test_ref_branches.cpp
)
target_link_libraries(test_unit_ref hpactor hpactor_test_support GTest::gtest_main)
gtest_discover_tests(test_unit_ref)
