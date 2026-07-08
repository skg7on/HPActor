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

#include <hpactor/config/python_binding_config.hpp>

namespace hpactor::config {

namespace {

constexpr bool is_power_of_two(uint32_t v) noexcept {
    return v != 0 && (v & (v - 1)) == 0;
}

constexpr bool valid_queue_cap(uint32_t v) noexcept {
    return v >= 64 && v <= 1'048'576 && is_power_of_two(v);
}

constexpr bool valid_budget(uint32_t v) noexcept {
    return v >= 1 && v <= 4096;
}

} // namespace

result<void> PythonBindingConfig::validate() const noexcept {
    if (!valid_queue_cap(dispatch_queue_capacity)) {
        return result<void>::make(
            error(errors::invalid_argument, "dispatch_queue_capacity invalid"));
    }
    if (!valid_queue_cap(command_queue_capacity)) {
        return result<void>::make(
            error(errors::invalid_argument, "command_queue_capacity invalid"));
    }
    if (!valid_queue_cap(completion_queue_capacity)) {
        return result<void>::make(error(errors::invalid_argument,
                                        "completion_queue_capacity invalid"));
    }
    if (!valid_budget(max_dispatch_per_tick)) {
        return result<void>::make(
            error(errors::invalid_argument, "max_dispatch_per_tick invalid"));
    }
    if (!valid_budget(max_commands_per_turn)) {
        return result<void>::make(
            error(errors::invalid_argument, "max_commands_per_turn invalid"));
    }
    if (max_dispatch_per_tick > dispatch_queue_capacity) {
        return result<void>::make(
            error(errors::invalid_argument,
                  "max_dispatch_per_tick exceeds dispatch_queue_capacity"));
    }
    if (max_commands_per_turn > command_queue_capacity) {
        return result<void>::make(
            error(errors::invalid_argument,
                  "max_commands_per_turn exceeds command_queue_capacity"));
    }
    if (max_actor_bindings < 1 || max_actor_bindings > 1'048'576) {
        return result<void>::make(
            error(errors::invalid_argument, "max_actor_bindings invalid"));
    }
    if (loop_lag_unready_ms < 100 || loop_lag_unready_ms > 60'000) {
        return result<void>::make(
            error(errors::invalid_argument, "loop_lag_unready_ms invalid"));
    }
    if (handler_shutdown_timeout_ms < 100 || handler_shutdown_timeout_ms > 300'000) {
        return result<void>::make(error(errors::invalid_argument,
                                        "handler_shutdown_timeout_ms invalid"));
    }
    return result<void>::make();
}

} // namespace hpactor::config
