# =============================================================================
# src/etcd/CMakeLists.txt — etcd v3 vendored protos and gRPC codegen
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
# =============================================================================

find_package(Protobuf REQUIRED)

# gRPC's find_dependency(Protobuf CONFIG) needs ProtobufConfig.cmake.
# When protobuf is found via MODULE mode, provide a minimal shim.
# This must be created BEFORE find_package(gRPC CONFIG) so gRPC can
# resolve its Protobuf dependency.
if(NOT EXISTS "${Protobuf_DIR}/ProtobufConfig.cmake")
  file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/cmake/protobuf-shim")
  file(WRITE "${CMAKE_BINARY_DIR}/cmake/protobuf-shim/ProtobufConfig.cmake"
    "# Shim: protobuf targets already exist from FindProtobuf.cmake\n")
  file(WRITE "${CMAKE_BINARY_DIR}/cmake/protobuf-shim/ProtobufConfigVersion.cmake"
    "set(PACKAGE_VERSION_COMPATIBLE TRUE)\nset(PACKAGE_VERSION_EXACT TRUE)\n")
  set(Protobuf_DIR "${CMAKE_BINARY_DIR}/cmake/protobuf-shim"
      CACHE PATH "Protobuf CONFIG shim for gRPC compatibility" FORCE)
endif()

# Try to find gRPC — optional, only needed for etcd backend
find_package(gRPC CONFIG QUIET)

add_library(hpactor_etcd STATIC)
target_include_directories(hpactor_etcd PUBLIC
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_SOURCE_DIR}/include
    ${Protobuf_INCLUDE_DIRS}
)

if(gRPC_FOUND)

    # Resolve grpc_cpp_plugin path
    if(TARGET gRPC::grpc_cpp_plugin)
        get_target_property(_configs gRPC::grpc_cpp_plugin IMPORTED_CONFIGURATIONS)
        foreach(_cfg ${_configs})
          get_target_property(_loc gRPC::grpc_cpp_plugin IMPORTED_LOCATION_${_cfg})
          if(_loc AND EXISTS "${_loc}")
            set(gRPC_CPP_PLUGIN "${_loc}")
            break()
          endif()
        endforeach()
        unset(_configs)
        unset(_loc)
        unset(_cfg)
    endif()
    if(NOT gRPC_CPP_PLUGIN)
        find_program(gRPC_CPP_PLUGIN grpc_cpp_plugin)
    endif()

    set(PROTO_FILES
        proto/kv.proto
        proto/rpc.proto
    )
    protobuf_generate(TARGET hpactor_etcd PROTOS ${PROTO_FILES} APPEND_PATH)
    protobuf_generate(
        TARGET hpactor_etcd
        PROTOS ${PROTO_FILES}
        LANGUAGE grpc
        GENERATE_EXTENSIONS .grpc.pb.h .grpc.pb.cc
        PLUGIN "protoc-gen-grpc=${gRPC_CPP_PLUGIN}"
        APPEND_PATH
    )
    target_link_libraries(hpactor_etcd PUBLIC protobuf::libprotobuf gRPC::grpc++)
    target_compile_definitions(hpactor_etcd PUBLIC HPACTOR_HAS_GRPC=1)
else()
    message(STATUS "gRPC not found — etcd backend will be unavailable (stub only)")
endif()

target_sources(hpactor_etcd PRIVATE etcd_serialize.cpp)
target_compile_options(hpactor_etcd PRIVATE -fexceptions)
target_compile_definitions(hpactor_etcd PUBLIC GOOGLE_PROTOBUF_NO_RTTI=1)
