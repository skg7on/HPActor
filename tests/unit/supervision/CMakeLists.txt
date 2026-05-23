add_executable(test_unit_supervision
    test_one_for_one_supervisor.cpp
    test_all_for_one_supervisor.cpp
)
target_link_libraries(test_unit_supervision hpactor GTest::gtest_main)
gtest_discover_tests(test_unit_supervision)
