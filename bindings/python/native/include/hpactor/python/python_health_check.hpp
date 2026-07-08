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
#include <string>

namespace hpactor::python {

// Forward declaration to avoid heavy include.
class PythonRuntime;
struct PythonRuntimeSnapshot;
enum class PythonRuntimeState : uint8_t;

/// \brief Health check for the Python runtime that reads snapshot-only state.
///
/// Returns Healthy when the runtime is Running with a fresh heartbeat.
/// Queue pressure alone does not make the node unready.
class PythonRuntimeHealthCheck final {
  public:
    /// \brief Construct with a pointer to the Python runtime.
    ///
    /// A null pointer means the binding is disabled; returns Healthy.
    explicit PythonRuntimeHealthCheck(const PythonRuntime* runtime) noexcept;

    /// \brief Name of this health check.
    [[nodiscard]] std::string name() const noexcept;

    /// \brief Whether this check is critical.
    [[nodiscard]] bool is_critical() const noexcept;

    /// \brief Execute the health check.
    ///
    /// \param[in] loop_lag_unready_ms Maximum allowed loop lag before unready.
    /// \return An error if unhealthy, or ok() if healthy.
    [[nodiscard]] result<void> check(uint32_t loop_lag_unready_ms) const noexcept;

  private:
    const PythonRuntime* runtime_{nullptr};
};

} // namespace hpactor::python
