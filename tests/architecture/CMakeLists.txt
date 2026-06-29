# Smoke test
add_test(
    NAME ActorSystemArchitectureScript.SmokeRejectsKnownToken
    COMMAND ${CMAKE_COMMAND}
        -DINPUT_FILE=${CMAKE_CURRENT_SOURCE_DIR}/test_actor_system_public_header.cpp
        -DFORBIDDEN=THIS_TOKEN_DOES_NOT_EXIST
        -P ${CMAKE_CURRENT_SOURCE_DIR}/assert_file_excludes.cmake
)

# Architecture: ActorSystem header is pointer-only after Phase 1b.
# Each token is a data field declaration pattern that must NOT appear.
set(_forbidden "Config config_" "EndPoint endpoint_" "Clock clock_" "ActorDirectory actor_directory_" "ActorRegistry registry_")
add_test(
    NAME ActorSystemFacadeIsPointerOnly
    COMMAND ${CMAKE_COMMAND}
        -DINPUT_FILE=${CMAKE_SOURCE_DIR}/include/hpactor/actor/actor_system.hpp
        "-DFORBIDDEN=${_forbidden}"
        -P ${CMAKE_CURRENT_SOURCE_DIR}/assert_file_excludes.cmake
)

# ── MessagingRuntime architecture fitness ───────────────────────────────────

# Check 1: No ActorSystem*/&/Impl* captures in messaging production code.
# Production messaging components must use concrete dependencies, not the
# facade or its private state.
set(_forbidden "ActorSystem*" "ActorSystem&" "Impl*" "Impl&")
set(_messaging_files
    ${CMAKE_SOURCE_DIR}/src/runtime/messaging_runtime.hpp
    ${CMAKE_SOURCE_DIR}/src/runtime/messaging_runtime.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/messaging_network_ports.hpp
    ${CMAKE_SOURCE_DIR}/include/hpactor/mailbox/delivery_pipeline.hpp
    ${CMAKE_SOURCE_DIR}/src/mailbox/delivery_pipeline.cpp
    ${CMAKE_SOURCE_DIR}/include/hpactor/mailbox/backpressure_coordinator.hpp
    ${CMAKE_SOURCE_DIR}/src/mailbox/backpressure_coordinator.cpp
    ${CMAKE_SOURCE_DIR}/include/hpactor/mailbox/local_delivery_engine.hpp
    ${CMAKE_SOURCE_DIR}/src/mailbox/local_delivery_engine.cpp)
foreach(_file IN LISTS _messaging_files)
    add_test(
        NAME MessagingRuntime.NoFacadeCapture.${_file}
        COMMAND ${CMAKE_COMMAND}
            -DINPUT_FILE=${_file}
            "-DFORBIDDEN=${_forbidden}"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/assert_file_excludes.cmake
    )
endforeach()

# Check 2: No late dependency setters in production messaging code.
# set_metrics_ring_buffer and set_transport were removed from production.
set(_forbidden "set_metrics_ring_buffer" "set_transport" "set_metrics(")
add_test(
    NAME MessagingRuntime.NoLateSettersInMessaging
    COMMAND ${CMAKE_COMMAND}
        -DINPUT_FILE=${CMAKE_SOURCE_DIR}/src/runtime/messaging_runtime.cpp
        "-DFORBIDDEN=${_forbidden}"
        -P ${CMAKE_CURRENT_SOURCE_DIR}/assert_file_excludes.cmake
)
add_test(
    NAME MessagingRuntime.NoLateSettersInPipeline
    COMMAND ${CMAKE_COMMAND}
        -DINPUT_FILE=${CMAKE_SOURCE_DIR}/include/hpactor/mailbox/delivery_pipeline.hpp
        "-DFORBIDDEN=${_forbidden}"
        -P ${CMAKE_CURRENT_SOURCE_DIR}/assert_file_excludes.cmake
)
add_test(
    NAME MessagingRuntime.NoLateSettersInBackpressure
    COMMAND ${CMAKE_COMMAND}
        -DINPUT_FILE=${CMAKE_SOURCE_DIR}/include/hpactor/mailbox/backpressure_coordinator.hpp
        "-DFORBIDDEN=${_forbidden}"
        -P ${CMAKE_CURRENT_SOURCE_DIR}/assert_file_excludes.cmake
)

# Check 3: No RTTI/exceptions in messaging runtime files.
set(_forbidden "dynamic_cast" "typeid" "throw " "try {" "catch (")
add_test(
    NAME MessagingRuntime.NoRttiInRuntime
    COMMAND ${CMAKE_COMMAND}
        -DINPUT_FILE=${CMAKE_SOURCE_DIR}/src/runtime/messaging_runtime.cpp
        "-DFORBIDDEN=${_forbidden}"
        -P ${CMAKE_CURRENT_SOURCE_DIR}/assert_file_excludes.cmake
)
add_test(
    NAME MessagingRuntime.NoRttiInRuntimeHeader
    COMMAND ${CMAKE_COMMAND}
        -DINPUT_FILE=${CMAKE_SOURCE_DIR}/src/runtime/messaging_runtime.hpp
        "-DFORBIDDEN=${_forbidden}"
        -P ${CMAKE_CURRENT_SOURCE_DIR}/assert_file_excludes.cmake
)
add_test(
    NAME MessagingRuntime.NoRttiInPorts
    COMMAND ${CMAKE_COMMAND}
        -DINPUT_FILE=${CMAKE_SOURCE_DIR}/src/runtime/messaging_network_ports.hpp
        "-DFORBIDDEN=${_forbidden}"
        -P ${CMAKE_CURRENT_SOURCE_DIR}/assert_file_excludes.cmake
)
