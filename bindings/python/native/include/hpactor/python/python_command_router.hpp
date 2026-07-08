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

#include <hpactor/python/python_ports.hpp>
#include <hpactor/python/python_runtime.hpp>

namespace hpactor {

class ActorSystem;

} // namespace hpactor

namespace hpactor::python {

/// \brief Converts queue commands into protected system-lane messages and
///        delivers them to the originating bridge actor for execution.
///
/// Installed as the \c PythonCommandExecutorPort on the gateway actor.
/// All actor-owned work (send, reply, ask, spawn, schedule, link, monitor,
/// stop, passivate) happens on the bridge actor's scheduler turn, not on
/// the Python event loop or the gateway actor's turn.
class PythonCommandRouter final {
  public:
    /// \brief Construct the router with the owning system and runtime.
    ///
    /// \param[in] system  The actor system used for message delivery.
    /// \param[in] runtime The Python runtime used for generation checks.
    PythonCommandRouter(ActorSystem& system, PythonRuntime& runtime) noexcept;

    PythonCommandRouter(const PythonCommandRouter&) = delete;
    PythonCommandRouter& operator=(const PythonCommandRouter&) = delete;

    /// \brief Return a fixed function-pointer port for the gateway actor.
    ///
    /// \return A non-owning \c PythonCommandExecutorPort referencing this
    ///         router.
    [[nodiscard]] PythonCommandExecutorPort port() noexcept;

  private:
    /// \brief Static entry point invoked by the gateway actor for each
    ///        command.
    ///
    /// Validates origin, generation, and routing constraints, then either
    /// delivers a protected command envelope to the bridge or returns an
    /// immediate error completion.
    ///
    /// \param[in] context Opaque pointer to this \c PythonCommandRouter.
    /// \param[in] command The command to route.
    /// \return Execution result with optional immediate completion.
    static PythonCommandExecution
    execute(void* context, const PythonCommand& command) noexcept;

    ActorSystem& system_;
    PythonRuntime& runtime_;
};

} // namespace hpactor::python
