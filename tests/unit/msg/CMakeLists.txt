add_executable(test_unit_msg
    test_retry_policy.cpp
    test_delivery_receipt.cpp
    test_ack_nack_frames.cpp
    test_outbound_delivery_tracker.cpp
    test_proto_registry.cpp
    test_stream_frames.cpp
)

target_link_libraries(test_unit_msg PRIVATE
    hpactor_lib
    hpactor_proto
    GTest::gtest
    GTest::gtest_main
)

gtest_discover_tests(test_unit_msg)
