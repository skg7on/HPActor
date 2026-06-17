add_executable(test_saturate_messages
    test_saturate_messages.cpp
)
target_link_libraries(test_saturate_messages PRIVATE hpactor_lib GTest::gtest GTest::gtest_main)
target_include_directories(test_saturate_messages PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/apps/bench_saturate
)
gtest_discover_tests(test_saturate_messages
    PROPERTIES
        LABELS "unit;saturate"
        TIMEOUT 10
)

add_executable(test_saturate_collector_math
    test_saturate_collector_math.cpp
)
target_link_libraries(test_saturate_collector_math PRIVATE hpactor_lib GTest::gtest GTest::gtest_main)
target_include_directories(test_saturate_collector_math PRIVATE ${CMAKE_SOURCE_DIR})
gtest_discover_tests(test_saturate_collector_math
    PROPERTIES
        LABELS "unit;saturate"
        TIMEOUT 10
)
