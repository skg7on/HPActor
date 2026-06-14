add_executable(test_unit_config
    test_actor_factory_registry.cpp
)
target_link_libraries(test_unit_config hpactor hpactor_test_support GTest::gtest_main)
gtest_discover_tests(test_unit_config)

if(ENABLE_AI_ACCELERATORS)
    add_executable(test_ai_accelerator_config
        test_ai_accelerator_config.cpp
    )
    target_compile_definitions(test_ai_accelerator_config
        PRIVATE TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/tests/data"
    )
    target_link_libraries(test_ai_accelerator_config
        PRIVATE hpactor GTest::gtest_main
    )
    gtest_discover_tests(test_ai_accelerator_config)
endif()
