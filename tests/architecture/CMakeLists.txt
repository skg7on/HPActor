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

# ── Phase 4: Frame & Stream routing architecture fitness ────────────────────

# Check 1: No ActorSystem*/&/Impl captures in router or stream runtime.
set(_phase4_no_facade "ActorSystem*" "ActorSystem&" "Impl*" "Impl&")
set(_phase4_sources
    ${CMAKE_SOURCE_DIR}/src/net/inbound_frame_router.hpp
    ${CMAKE_SOURCE_DIR}/src/net/inbound_frame_router.cpp
    ${CMAKE_SOURCE_DIR}/src/actor/stream_runtime.hpp
    ${CMAKE_SOURCE_DIR}/src/actor/stream_runtime.cpp)
foreach(_file IN LISTS _phase4_sources)
    add_test(
        NAME Phase4.NoFacadeCapture.${_file}
        COMMAND ${CMAKE_COMMAND}
            -DINPUT_FILE=${_file}
            "-DFORBIDDEN=${_phase4_no_facade}"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/assert_file_excludes.cmake
    )
endforeach()

# Check 2: WireFrameConnection handle_read has no recursive self-call.
# The recursive pattern was: handle_read(); with comment "Recurse to re-enter".
add_test(
    NAME Phase4.NoRecursiveFraming
    COMMAND ${CMAKE_COMMAND}
        -DINPUT_FILE=${CMAKE_SOURCE_DIR}/src/net/wireframe_connection.cpp
        "-DFORBIDDEN=Recurse to re-enter"
        -P ${CMAKE_CURRENT_SOURCE_DIR}/assert_file_excludes.cmake
)

# Check 3: No RTTI/exceptions in router and stream runtime.
set(_phase4_no_rtti "dynamic_cast" "typeid" "throw " "try {" "catch (")
foreach(_file2 IN LISTS _phase4_sources)
    add_test(
        NAME Phase4.NoRtti.${_file2}
        COMMAND ${CMAKE_COMMAND}
            -DINPUT_FILE=${_file2}
            "-DFORBIDDEN=${_phase4_no_rtti}"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/assert_file_excludes.cmake
    )
endforeach()

# ── Phase 5: NetworkRuntime architecture fitness ─────────────────────────────

# Check 1: No ActorSystem*/&/Impl captures in network runtime production code.
set(_phase5_no_facade "ActorSystem*" "ActorSystem&" "Impl*" "Impl&")
set(_phase5_sources
    ${CMAKE_SOURCE_DIR}/include/hpactor/runtime/network_runtime.hpp
    ${CMAKE_SOURCE_DIR}/src/runtime/network_runtime.cpp
    ${CMAKE_SOURCE_DIR}/include/hpactor/runtime/network_runtime_callbacks.hpp
    ${CMAKE_SOURCE_DIR}/include/hpactor/net/network_snapshot.hpp)
foreach(_file IN LISTS _phase5_sources)
    add_test(
        NAME Phase5.NoFacadeCapture.${_file}
        COMMAND ${CMAKE_COMMAND}
            -DINPUT_FILE=${_file}
            "-DFORBIDDEN=${_phase5_no_facade}"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/assert_file_excludes.cmake
    )
endforeach()

# Check 2: No RTTI/exceptions in network runtime files.
set(_phase5_no_rtti "dynamic_cast" "typeid" "throw " "try {" "catch (")
foreach(_file2 IN LISTS _phase5_sources)
    add_test(
        NAME Phase5.NoRtti.${_file2}
        COMMAND ${CMAKE_COMMAND}
            -DINPUT_FILE=${_file2}
            "-DFORBIDDEN=${_phase5_no_rtti}"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/assert_file_excludes.cmake
    )
endforeach()

# Check 3: No late dependency setters in NetworkRuntime.
# Dependencies are fixed at construction.
add_test(
    NAME Phase5.NoLateSettersInNetworkRuntime
    COMMAND ${CMAKE_COMMAND}
        -DINPUT_FILE=${CMAKE_SOURCE_DIR}/include/hpactor/runtime/network_runtime.hpp
        "-DFORBIDDEN=set_actor_system;set_scheduler;set_transport;set_discovery;set_metrics("
        -P ${CMAKE_CURRENT_SOURCE_DIR}/assert_file_excludes.cmake
)

# Check 4: No second EventLoop creation in NetworkRuntime.
# The TcpTransport owns the sole authoritative loop.
add_test(
    NAME Phase5.NoSecondEventLoop
    COMMAND ${CMAKE_COMMAND}
        -DINPUT_FILE=${CMAKE_SOURCE_DIR}/src/runtime/network_runtime.cpp
        "-DFORBIDDEN=make_unique<net::EventLoop>;new net::EventLoop;std::make_unique<net::EventLoop>"
        -P ${CMAKE_CURRENT_SOURCE_DIR}/assert_file_excludes.cmake
)

# Check 5: No std::function callbacks that capture the facade in network files.
# All callbacks use fixed function-pointer ports.
add_test(
    NAME Phase5.NoFacadeCallbacks
    COMMAND ${CMAKE_COMMAND}
        -DINPUT_FILE=${CMAKE_SOURCE_DIR}/src/runtime/network_runtime.cpp
        "-DFORBIDDEN=std::function"
        -P ${CMAKE_CURRENT_SOURCE_DIR}/assert_file_excludes.cmake
)

# ── Phase 6: Runtime lifecycle architecture fitness ──────────────────────────

# Check 1: No RTTI/exceptions in runtime lifecycle files.
set(_phase6_no_rtti "dynamic_cast" "typeid" "throw " "try {" "catch (")
set(_phase6_sources
    ${CMAKE_SOURCE_DIR}/src/runtime/runtime_blueprint.hpp
    ${CMAKE_SOURCE_DIR}/src/runtime/runtime_blueprint_builder.hpp
    ${CMAKE_SOURCE_DIR}/src/runtime/runtime_blueprint_builder.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/runtime_builder.hpp
    ${CMAKE_SOURCE_DIR}/src/runtime/runtime_builder.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/runtime_coordinator.hpp
    ${CMAKE_SOURCE_DIR}/src/runtime/runtime_coordinator.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/runtime_startup.hpp
    ${CMAKE_SOURCE_DIR}/src/runtime/runtime_startup.cpp)
foreach(_file IN LISTS _phase6_sources)
    add_test(
        NAME Phase6.NoRtti.${_file}
        COMMAND ${CMAKE_COMMAND}
            -DINPUT_FILE=${_file}
            "-DFORBIDDEN=${_phase6_no_rtti}"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/assert_file_excludes.cmake
    )
endforeach()

# Check 2: No std::function callbacks in coordinator (fixed ports only).
add_test(
    NAME Phase6.NoStdFunctionInCoordinator
    COMMAND ${CMAKE_COMMAND}
        -DINPUT_FILE=${CMAKE_SOURCE_DIR}/src/runtime/runtime_coordinator.hpp
        "-DFORBIDDEN=std::function"
        -P ${CMAKE_CURRENT_SOURCE_DIR}/assert_file_excludes.cmake
)
