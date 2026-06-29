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
