# Test: smoke-check that assert_file_excludes correctly reports a known token
add_test(
    NAME ActorSystemArchitectureScript.SmokeRejectsKnownToken
    COMMAND ${CMAKE_COMMAND}
        -DINPUT_FILE=${CMAKE_CURRENT_SOURCE_DIR}/test_actor_system_public_header.cpp
        -DFORBIDDEN=THIS_TOKEN_DOES_NOT_EXIST
        -P ${CMAKE_CURRENT_SOURCE_DIR}/assert_file_excludes.cmake
)

# The real exclusion tests will be added in subsequent tasks as state groups
# are moved behind Impl.
