add_executable(test_unit_adt
    test_adt_mpsc_ring_buffer.cpp
)
target_link_libraries(test_unit_adt hpactor hpactor_test_support GTest::gtest_main)
gtest_discover_tests(test_unit_adt)
