add_executable(test_unit_mailbox
    test_message.cpp
    test_message_advanced.cpp
    test_mailbox_interface.cpp
    test_mailbox_factory.cpp
    test_mutex_mailbox.cpp
    test_mailbox_stress.cpp
    test_bounded_mailbox.cpp
    test_mailbox_overflow_policies.cpp
    test_mailbox_overflow_queue.cpp
    test_mailbox_policy.cpp
    test_dead_letter_failure.cpp
    test_dead_letter_queue.cpp
    test_delivery_result.cpp
    test_delivery_mode.cpp
    test_is_expired.cpp
    test_dedup_cache.cpp
    test_backpressure_signal_serialization.cpp
    test_reservation_manager.cpp
    test_pressure_state_machine.cpp
    test_backpressure_signal_gate.cpp
    test_overflow_handlers.cpp
    test_overflow_handler_factory.cpp
    test_multi_lane_queue.cpp
    test_priority_lanes.cpp
    test_actor_rate_limiter.cpp
    test_rate_limiter_mailbox.cpp
    test_admission_policies.cpp
    test_admission_mailbox.cpp
    test_mailbox_formal_validation.cpp
    test_mailbox_stress_formal.cpp
    test_prearm_race.cpp
    test_stash_buffer.cpp
)
target_link_libraries(test_unit_mailbox hpactor hpactor_test_support GTest::gtest_main)
gtest_discover_tests(test_unit_mailbox)

# Relacy-based exhaustive MPSC queue verification.
# Tests the actual MPSCMailbox<T> template from mpsc_mailbox.hpp.
#
# fakestd/ is added BEFORE system includes so that #include <atomic>
# resolves to Relacy's fake atomic header instead of the system one.
# This avoids ODR conflicts between rl::atomic and std::atomic while
# still providing full instrumentation.
if(ENABLE_RELACY_TESTS)
    add_executable(test_mpsc_relacy
        test_mpsc_relacy.cpp
    )
    target_include_directories(test_mpsc_relacy BEFORE PRIVATE
        ${CMAKE_SOURCE_DIR}/third_party/relacy/include/relacy/fakestd
    )
    target_include_directories(test_mpsc_relacy SYSTEM PRIVATE
        ${CMAKE_SOURCE_DIR}/third_party/relacy/include
    )
    target_include_directories(test_mpsc_relacy PRIVATE
        ${CMAKE_SOURCE_DIR}/include
    )
    # Relacy uses typeid and throw internally — override the project-wide
    # -fno-rtti and -fno-exceptions for this target.
    target_compile_options(test_mpsc_relacy PRIVATE
        -fexceptions
        -frtti
        -Wno-error=missing-field-initializers
        -Wno-error=conversion
        -Wno-error=return-type
    )
endif()
