add_executable(test_unit_runtime
    test_network_runtime_lifecycle.cpp
    test_runtime_lifecycle_boundaries.cpp
    test_runtime_blueprint.cpp
    test_reload_classification.cpp
    test_runtime_builder.cpp
    test_runtime_coordinator.cpp
    test_coordinated_startup.cpp
)

target_include_directories(test_unit_runtime PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_link_libraries(test_unit_runtime PRIVATE
    hpactor_lib
    hpactor_proto
    GTest::gtest
    GTest::gtest_main
)

gtest_discover_tests(test_unit_runtime)
