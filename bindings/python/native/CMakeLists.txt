# Copyright 2026 HPActor Contributors
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

if(HPACTOR_PYTHON_WHEEL_BUILD)
  if(CMAKE_VERSION VERSION_LESS 3.26)
    message(FATAL_ERROR
      "Python ABI3 wheel builds require CMake 3.26 or newer")
  endif()
  find_package(Python3 3.11 REQUIRED
    COMPONENTS Interpreter Development.SABIModule)
else()
  find_package(Python3 COMPONENTS Development)
endif()

add_library(hpactor_python_native STATIC
    src/python_bridge_actor.cpp
    src/python_command_codec.cpp
    src/python_command_router.cpp
    src/python_health_check.cpp
    src/python_inspection.cpp
    src/python_native_system.cpp
    src/python_observability.cpp
    src/python_reliability.cpp
    src/python_runtime_config.cpp
    src/python_runtime_queues.cpp
    src/python_runtime.cpp)
set_target_properties(hpactor_python_native PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(hpactor_python_native PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(hpactor_python_native PUBLIC hpactor_lib hpactor_proto)
target_compile_options(hpactor_python_native PRIVATE -fno-exceptions -fno-rtti)
target_sources(hpactor_python_native PRIVATE
    src/native_notifier.cpp
    src/python_gateway_actor.cpp
    src/python_gateway_wake_adapter.cpp)

# ── CPython limited-API extension module ──────────────────────────────────
if(Python3_FOUND OR Python3_Development_FOUND)
  set(PYTHON_CAPI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/python_capi)

  add_library(_hpactor MODULE
      ${PYTHON_CAPI_DIR}/module.cpp
      ${PYTHON_CAPI_DIR}/native_system_type.cpp
      ${PYTHON_CAPI_DIR}/conversions.cpp)
  set_target_properties(_hpactor PROPERTIES
    PREFIX ""
    OUTPUT_NAME "_hpactor"
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
  target_compile_definitions(_hpactor PRIVATE Py_LIMITED_API=0x030B0000)
  target_include_directories(_hpactor PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/include
      ${Python3_INCLUDE_DIRS})
  if(HPACTOR_PYTHON_WHEEL_BUILD)
    target_link_libraries(_hpactor PRIVATE hpactor_python_native hpactor_lib
        hpactor_proto Python3::SABIModule)
  else()
    target_link_libraries(_hpactor PRIVATE hpactor_python_native hpactor_lib
        hpactor_proto Python3::Python)
  endif()
  target_compile_options(_hpactor PRIVATE -fno-exceptions -fno-rtti)
endif()
