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

#include <hpactor/types/types.hpp>

#include <cstdint>

namespace hpactor::config {

/// \brief Python language binding configuration parsed from [system.python].
///
/// All queue capacities must be powers of two in the range [64, 1048576].
/// Drain budgets must be in [1, 4096] and not exceed their queue capacity.
/// Actor bindings must be in [1, 1048576].
/// Loop lag must be in [100, 60000] ms.
/// Shutdown timeout must be in [100, 300000] ms.
struct PythonBindingConfig final {
    bool enabled{false};
    uint32_t dispatch_queue_capacity{65536};
    uint32_t command_queue_capacity{16384};
    uint32_t completion_queue_capacity{16384};
    uint32_t max_actor_bindings{65536};
    uint32_t max_dispatch_per_tick{256};
    uint32_t max_commands_per_turn{256};
    uint32_t loop_lag_unready_ms{5000};
    uint32_t handler_shutdown_timeout_ms{10000};
    bool trace_handler_spans{true};

    /// \brief Validate all bounded fields.
    ///
    /// \return ok() on success, or an error describing the first violation.
    [[nodiscard]] result<void> validate() const noexcept;
};

} // namespace hpactor::config
