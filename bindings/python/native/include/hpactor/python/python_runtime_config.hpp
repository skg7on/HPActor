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

#pragma once

#include <cstddef>
#include <cstdint>

namespace hpactor::python {

/// \brief Validation error codes for PythonRuntimeConfig.
enum class PythonConfigError : uint8_t {
    None,          ///< Configuration is valid.
    QueueCapacity, ///< Queue capacity is not a power of two, or out of
                   ///< range [64, 1048576].
    DrainBudget,   ///< Drain budget is zero, or exceeds its corresponding
                   ///< queue capacity.
    ActorBindingCapacity, ///< max_actor_bindings is out of range [1, 1048576].
    LoopLag,              ///< loop_lag_unready_ms is out of range [100, 60000].
    ShutdownTimeout,      ///< handler_shutdown_timeout_ms is out of range [100,
                          ///< 300000].
};

/// \brief Validates that a queue capacity is a power of two within the allowed
///        inclusive range [64, 1048576].
///
/// \param[in] value The capacity to validate.
/// \return true if value is a power of two in [64, 1048576].
constexpr bool valid_queue_capacity(size_t value) noexcept {
    return value >= 64 && value <= 1'048'576 && (value & (value - 1)) == 0;
}

/// \brief Runtime configuration for the Python bridge subsystem.
///
/// All queue capacities must be powers of two in [64, 1048576]. Drain budgets
/// must be in [1, 4096] and cannot exceed the corresponding queue capacity.
struct PythonRuntimeConfig final {
    /// Capacity of the dispatch queue (power of two, [64, 1048576]).
    size_t dispatch_queue_capacity{65'536};

    /// Capacity of the command queue (power of two, [64, 1048576]).
    size_t command_queue_capacity{16'384};

    /// Capacity of the completion queue (power of two, [64, 1048576]).
    size_t completion_queue_capacity{16'384};

    /// Maximum number of Python actor bindings [1, 1048576].
    size_t max_actor_bindings{65'536};

    /// Maximum dispatch dequeues per gateway tick [1, 4096].
    size_t max_dispatch_per_tick{256};

    /// Maximum commands processed per gateway turn [1, 4096].
    size_t max_commands_per_turn{256};

    /// Maximum time (ms) to stay unready when lag is detected [100, 60000].
    uint32_t loop_lag_unready_ms{5'000};

    /// Maximum time (ms) to wait for handler shutdown [100, 300000].
    uint32_t handler_shutdown_timeout_ms{10'000};

    /// When true, handler dispatch spans are traced.
    bool trace_handler_spans{true};

    /// \brief Validate all configuration fields.
    ///
    /// \return PythonConfigError::None if valid, or the first error detected.
    [[nodiscard]] PythonConfigError validate() const noexcept;
};

} // namespace hpactor::python
