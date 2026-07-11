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

find_package(Python3 COMPONENTS Development)

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
    src/python_runtime.cpp
    src/python_topology_types.cpp
    src/python_topology_preparer.cpp)
set_target_properties(hpactor_python_native PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(hpactor_python_native PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)

# Wheel builds keep linkage private so the extension is self-contained.
# Normal SDK builds keep public linkage for downstream consumers.
if(HPACTOR_PYTHON_WHEEL_BUILD)
  target_link_libraries(hpactor_python_native PRIVATE hpactor_lib hpactor_proto)
else()
  target_link_libraries(hpactor_python_native PUBLIC hpactor_lib hpactor_proto)
endif()

target_compile_options(hpactor_python_native PRIVATE -fno-exceptions -fno-rtti)
target_sources(hpactor_python_native PRIVATE
    src/native_notifier.cpp
    src/python_gateway_actor.cpp
    src/python_gateway_wake_adapter.cpp)

# ── pybind11 extension module ─────────────────────────────────────────
if(Python3_FOUND OR Python3_Development_FOUND)
  set(PYBIND11_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/python_pybind11)

  add_library(_hpactor MODULE
      ${PYBIND11_SRC_DIR}/module.cpp
      ${PYBIND11_SRC_DIR}/native_system.cpp)
  set_target_properties(_hpactor PROPERTIES
      PREFIX ""
      OUTPUT_NAME "_hpactor"
      CXX_VISIBILITY_PRESET hidden
      VISIBILITY_INLINES_HIDDEN ON)

  # NOTE: Py_LIMITED_API / PYBIND11_USE_LIMITED_API are deferred.
  # pybind11 2.13.6 has incomplete limited-API support for our usage
  # (PyCFunction_GET_SELF is exposed as a function, not a macro).
  # ABI3 compliance will be verified by abi3audit in Task 4.
  # The extension compiles against the full CPython API for now;
  # the stable ABI tag is asserted at audit time, not at compile time.

  target_include_directories(_hpactor PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/include
      ${PYBIND11_SRC_DIR})
  target_include_directories(_hpactor SYSTEM PRIVATE
      ${CMAKE_SOURCE_DIR}/third_party/pybind11/include
      ${Python3_INCLUDE_DIRS})
  target_link_libraries(_hpactor PRIVATE hpactor_python_native hpactor_lib
      hpactor_proto Python3::Python)
  target_compile_options(_hpactor PRIVATE -fexceptions -frtti)
endif()
