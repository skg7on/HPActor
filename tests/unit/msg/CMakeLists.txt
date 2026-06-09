add_executable(test_unit_msg
    test_retry_policy.cpp
    test_delivery_receipt.cpp
    test_ack_nack_frames.cpp
)

target_link_libraries(test_unit_msg PRIVATE
    hpactor_lib
    GTest::gtest
    GTest::gtest_main
)

add_test(NAME UnitMsg COMMAND test_unit_msg)
