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
    explicit PythonShutdownAdapter(uint32_t handler_shutdown_timeout_ms) noexcept;

    /// \brief Begin the shutdown sequence for the given runtime.
    [[nodiscard]] result<void> execute(PythonRuntime& runtime) noexcept;

    /// \brief Whether Python objects have been quiesced.
    [[nodiscard]] bool python_objects_quiesced() const noexcept;

  private:
    uint32_t handler_shutdown_timeout_ms_;
    std::atomic<bool> python_objects_quiesced_{false};
};

} // namespace hpactor::python
