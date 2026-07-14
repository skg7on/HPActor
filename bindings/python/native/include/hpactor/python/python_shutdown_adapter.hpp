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

#include <atomic>
#include <cstdint>

namespace hpactor::python {

class PythonRuntime;

/// \brief Coordinates Python runtime shutdown in the approved 10-step order.
class PythonShutdownAdapter final {
  public:
    /// \brief Construct the shutdown adapter.
    ///
    /// \param[in] handler_shutdown_timeout_ms Maximum time to wait for each
    ///            Python handler to complete during shutdown (100..300000 ms).
    explicit PythonShutdownAdapter(uint32_t handler_shutdown_timeout_ms) noexcept;

    /// \brief Begin the shutdown sequence for the given runtime.
    ///
    /// Executes the approved 10-step shutdown order: drain dispatch, drain
    /// commands, quiesce Python objects, stop runtime, stop bridges, stop
    /// gateway, stop actor system.
    ///
    /// \param[in,out] runtime The Python runtime to shut down. Must outlive
    ///                        this adapter.
    /// \return ok() on success, or an error if shutdown timed out or failed
    ///         at an intermediate step.
    /// \note Thread safety: must be called from the shutdown coordinator
    ///       thread. Not safe for concurrent invocation.
    [[nodiscard]] result<void> execute(PythonRuntime& runtime) noexcept;

    /// \brief Whether Python objects have been quiesced.
    ///
    /// \return true if the Python-side object graph has been released and no
    ///         further callbacks will fire into Python code.
    /// \note Thread safety: safe to read from any thread after execute()
    ///       has returned.
    [[nodiscard]] bool python_objects_quiesced() const noexcept;

  private:
    uint32_t handler_shutdown_timeout_ms_;
    std::atomic<bool> python_objects_quiesced_{false};
};

} // namespace hpactor::python
