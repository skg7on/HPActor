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

#include <hpactor/python/python_bridge_types.hpp>

#include <cstddef>

namespace hpactor::python {

/// \brief Fixed function-pointer port for waking the native gateway actor when
///        commands are available on the command queue.
///
/// Uses a raw function pointer and opaque context pointer instead of
/// std::function to avoid exception-throwing paths and heap allocations in the
/// hot path.
struct GatewayWakePort final {
    /// Opaque context passed as the first argument to \ref wake.
    void* context{nullptr};

    /// \brief Wake callback invoked when the command queue transitions from
    ///        empty to non-empty.
    ///
    /// \param[in] ctx The opaque \ref context pointer.
    /// \return true if the wake signal was delivered to the gateway actor.
    bool (*wake)(void* ctx) noexcept {nullptr};

    /// \brief Check whether this port holds a valid wake callback.
    ///
    /// \return true if both \ref context and \ref wake are non-null.
    [[nodiscard]] explicit operator bool() const noexcept {
        return context != nullptr && wake != nullptr;
    }
};

/// \brief Result of a single command execution on the fixed executor port.
///
/// The executor returns this to indicate whether a completion should be
/// emitted back to the Python interpreter thread.
struct PythonCommandExecution final {
    /// When true, \ref completion is valid and will be pushed to the
    /// completion queue.
    bool emit_completion{false};

    /// The completion record to push when \ref emit_completion is true.
    PythonCompletion completion;
};

/// \brief Fixed function-pointer port for executing Python commands.
///
/// Uses a raw function pointer and opaque context pointer instead of
/// std::function to avoid exception-throwing paths and heap allocations
/// in the hot path.
struct PythonCommandExecutorPort final {
    /// Opaque context passed as the first argument to \ref execute.
    void* context{nullptr};

    /// \brief Execute a command from the Python interpreter.
    ///
    /// \param[in] ctx The opaque \ref context pointer.
    /// \param[in] command The command to execute.
    /// \return A \c PythonCommandExecution describing whether a completion
    ///         should be emitted.
    PythonCommandExecution (*execute)(void* ctx,
                                      const PythonCommand& command) noexcept {nullptr};

    /// \brief Check whether this port holds a valid executor.
    ///
    /// \return true if both \ref context and \ref execute are non-null.
    [[nodiscard]] explicit operator bool() const noexcept {
        return context != nullptr && execute != nullptr;
    }
};

} // namespace hpactor::python
