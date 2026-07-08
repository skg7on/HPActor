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

add_library(hpactor_python_native STATIC
    src/python_bridge_actor.cpp
    src/python_command_codec.cpp
    src/python_command_router.cpp
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
