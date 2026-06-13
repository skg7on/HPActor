# =============================================================================
# dependencies.cmake — third-party packages, protobuf codegen, vendored libs
# =============================================================================
#
# Included from the top-level CMakeLists.txt (same scope).  Sets:
#   PROTO_SRCS, PROTO_HDRS, HPACTOR_PROTO_ABSL_LIBS, HPACTOR_ABSL_LIBS,
#   hpactor_proto target, linenoise target, toml++ interface target.

# ---- Protobuf + Abseil -----------------------------------------------------

find_package(Protobuf REQUIRED)

execute_process(COMMAND ${Protobuf_PROTOC_EXECUTABLE} --version
    OUTPUT_VARIABLE HPACTOR_PROTOC_VERSION_OUTPUT
    OUTPUT_STRIP_TRAILING_WHITESPACE)
string(REGEX MATCH "[0-9]+\\.[0-9]+" HPACTOR_PROTOC_VERSION "${HPACTOR_PROTOC_VERSION_OUTPUT}")
if(HPACTOR_PROTOC_VERSION VERSION_GREATER_EQUAL "22.0")
    find_package(absl REQUIRED)
    set(HPACTOR_PROTO_ABSL_LIBS absl::log_internal_check_op)
    set(HPACTOR_ABSL_LIBS absl::log absl::log_internal_check_op)
else()
    set(HPACTOR_PROTO_ABSL_LIBS "")
    set(HPACTOR_ABSL_LIBS "")
endif()

# ---- vendored libraries ----------------------------------------------------

# toml++ v3.4.0 (header-only)
include(${CMAKE_SOURCE_DIR}/cmake/tomlplusplus.cmake)

# protobuf codegen
include(${CMAKE_SOURCE_DIR}/cmake/protobuf.cmake)
PROTOBUF_GENERATE_CPP(PROTO_SRCS PROTO_HDRS
    ${CMAKE_SOURCE_DIR}/protos/hpactor/frame.proto
    ${CMAKE_SOURCE_DIR}/protos/hpactor/common.proto
    ${CMAKE_SOURCE_DIR}/protos/hpactor/messages.proto
    ${CMAKE_SOURCE_DIR}/protos/hpactor/registrar.proto
    ${CMAKE_SOURCE_DIR}/protos/hpactor/gossip.proto
    ${CMAKE_SOURCE_DIR}/protos/hpactor/cli_messages.proto
    ${CMAKE_SOURCE_DIR}/protos/hpactor/ai_resource.proto
)

add_library(hpactor_proto SHARED ${PROTO_SRCS} ${PROTO_HDRS})
set_target_properties(hpactor_proto PROPERTIES CXX_CLANG_TIDY "")
target_link_libraries(hpactor_proto PUBLIC protobuf::libprotobuf ${HPACTOR_PROTO_ABSL_LIBS})
target_compile_options(hpactor_proto PRIVATE -Wno-sign-conversion)
target_include_directories(hpactor_proto PUBLIC ${CMAKE_CURRENT_BINARY_DIR})

# linenoise — vendored line editing library
add_library(linenoise STATIC
    third_party/linenoise/linenoise.c)
set_target_properties(linenoise PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(linenoise PUBLIC third_party/linenoise)
target_compile_options(linenoise PRIVATE
    -Wno-error
    -Wno-conversion
    -Wno-sign-conversion
    -Wno-sign-compare
    -Wno-unused-parameter
    $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-implicit-int-conversion>
    $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-shorten-64-to-32>
    $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-extra-semi-stmt>
)

# ---- OpenSSL ---------------------------------------------------------------

find_package(OpenSSL REQUIRED)
