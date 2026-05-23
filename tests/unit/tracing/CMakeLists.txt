add_executable(test_unit_tracing
    test_trace_config_smoke.cpp
    test_trace_context.cpp
    test_w3c_trace_context.cpp
    test_sampler.cpp
    test_trace_typed_message.cpp
)
target_link_libraries(test_unit_tracing hpactor GTest::gtest_main)
gtest_discover_tests(test_unit_tracing)
