# =============================================================================
# src/CMakeLists.txt — hpactor_lib shared library and platform backends
# =============================================================================

add_library(hpactor_lib SHARED)

target_include_directories(hpactor_lib
    PUBLIC
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_BINARY_DIR}
        ${CMAKE_SOURCE_DIR}/third_party/llhttp
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src
)

# ---- vendored C sources (llhttp) -------------------------------------------

target_sources(hpactor_lib PRIVATE
    ../third_party/llhttp/llhttp.c
    ../third_party/llhttp/api.c
    ../third_party/llhttp/http.c
)

if(CMAKE_C_COMPILER_ID MATCHES "Clang|AppleClang")
    set(LLHTTP_CLANG_FLAGS "-Wno-implicit-int-conversion")
else()
    set(LLHTTP_CLANG_FLAGS "")
endif()

set_source_files_properties(
    ../third_party/llhttp/llhttp.c
    ../third_party/llhttp/api.c
    ../third_party/llhttp/http.c
    PROPERTIES
        LANGUAGE C
        COMPILE_FLAGS "-Wno-error -Wno-conversion -Wno-sign-conversion -Wno-unused-parameter -Wno-sign-compare -Wno-unused-variable ${LLHTTP_CLANG_FLAGS}"
)

# ---- subsystem sources -----------------------------------------------------

add_subdirectory(actor)
add_subdirectory(adt)
add_subdirectory(cli)
add_subdirectory(config)
add_subdirectory(coroutine)
add_subdirectory(fault)
add_subdirectory(log)
add_subdirectory(mailbox)
add_subdirectory(mem)
add_subdirectory(metrics)
add_subdirectory(msg)
add_subdirectory(net)
add_subdirectory(process)
add_subdirectory(ref)
add_subdirectory(runtime)
add_subdirectory(rpc)
add_subdirectory(sched)
add_subdirectory(supervision)
add_subdirectory(timer)
add_subdirectory(tracing)
add_subdirectory(types)

# ---- AI accelerator subsystem (compile-time gate, runtime opt-in) -----------

if(ENABLE_AI_ACCELERATORS)
    add_subdirectory(ai)
endif()

# ---- exceptions for toml++ TUs ---------------------------------------------

set_source_files_properties(
    config/toml_parser.cpp
    config/toml_table_view.cpp
    PROPERTIES COMPILE_FLAGS "-fexceptions"
)

# ---- link dependencies -----------------------------------------------------

target_link_libraries(hpactor_lib
    PUBLIC
        tomlplusplus::tomlplusplus
        OpenSSL::SSL
        OpenSSL::Crypto
        protobuf::libprotobuf
        ${HPACTOR_ABSL_LIBS}
        hpactor_proto
)

if(ENABLE_CLI)
    target_link_libraries(hpactor_lib PRIVATE linenoise)
endif()

# ARM64 / RISC-V may need -latomic for std::atomic<uint64_t>
if(UNIX AND NOT APPLE)
    target_link_libraries(hpactor_lib PUBLIC atomic)
endif()

# ---- alias -----------------------------------------------------------------

add_library(hpactor ALIAS hpactor_lib)

# ---- cluster subsystem (failure model, partitioning, route invalidation) ----

add_subdirectory(cluster)
