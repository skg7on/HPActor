# Verify that specific tokens do not appear in a file.
# Required variables: INPUT_FILE (path), FORBIDDEN (CMake list of strings).

if(NOT DEFINED INPUT_FILE)
    message(FATAL_ERROR "INPUT_FILE is required")
endif()

if(NOT EXISTS "${INPUT_FILE}")
    message(FATAL_ERROR "INPUT_FILE '${INPUT_FILE}' does not exist")
endif()

file(READ "${INPUT_FILE}" _file_content)

foreach(_token IN LISTS FORBIDDEN)
    string(FIND "${_file_content}" "${_token}" _pos)
    if(NOT _pos STREQUAL "-1")
        message(FATAL_ERROR
            "Architecture violation: forbidden token '${_token}' found in ${INPUT_FILE}")
    endif()
endforeach()

message(STATUS "Architecture check passed: no forbidden tokens in ${INPUT_FILE}")
