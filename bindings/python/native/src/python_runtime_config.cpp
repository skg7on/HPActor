// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <hpactor/python/python_runtime_config.hpp>

namespace hpactor::python {

PythonConfigError PythonRuntimeConfig::validate() const noexcept {
    if (!valid_queue_capacity(dispatch_queue_capacity) ||
        !valid_queue_capacity(command_queue_capacity) ||
        !valid_queue_capacity(completion_queue_capacity)) {
        return PythonConfigError::QueueCapacity;
    }

    if (max_dispatch_per_tick == 0 || max_dispatch_per_tick > 4096 ||
        max_dispatch_per_tick > dispatch_queue_capacity) {
        return PythonConfigError::DrainBudget;
    }

    if (max_commands_per_turn == 0 || max_commands_per_turn > 4096 ||
        max_commands_per_turn > command_queue_capacity) {
        return PythonConfigError::DrainBudget;
    }

    if (max_actor_bindings == 0 || max_actor_bindings > 1'048'576) {
        return PythonConfigError::ActorBindingCapacity;
    }

    if (loop_lag_unready_ms < 100 || loop_lag_unready_ms > 60'000) {
        return PythonConfigError::LoopLag;
    }

    if (handler_shutdown_timeout_ms < 100 || handler_shutdown_timeout_ms > 300'000) {
        return PythonConfigError::ShutdownTimeout;
    }

    return PythonConfigError::None;
}

} // namespace hpactor::python
