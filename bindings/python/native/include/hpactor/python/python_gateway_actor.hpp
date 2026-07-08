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

#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/msg/type_tag.hpp>
#include <hpactor/python/python_ports.hpp>
#include <hpactor/python/python_runtime.hpp>

#include <cstdint>
#include <string_view>

namespace hpactor::python {

/// \brief Local-only wakeup tag for the Python gateway actor.
///
/// Deliberately placed in the protected subsystem range (0x80–0xFF) so that
/// \c EventBasedActor::dispatch_system_message() routes it through the
/// system-message switch. The gateway intercepts this tag in its own
/// \c receive() override before the base class dispatch, ensuring it is
/// never accidentally consumed by another handler.
inline constexpr TypeTag kPythonWakeupTag = make_subsystem_tag(0xF0);

/// \brief Native gateway actor that drains the Python interpreter's command
///        queue with a per-turn budget and executes each command through a
///        fixed function-pointer port.
///
/// On each wakeup (triggered by a \c kPythonWakeupTag message), the gateway
/// drains at most \c max_commands_per_turn commands from the command queue.
/// Each command is executed through the \c PythonCommandExecutorPort. If the
/// executor requests a completion, it is pushed into the runtime's completion
/// queue. When commands remain after the budget is exhausted, the gateway
/// self-sends an empty \c kPythonWakeupTag message to requeue for another
/// scheduler turn.
///
/// \note Thread affinity: runs on the scheduler like any EventBasedActor.
///       The wake adapter is called from the Python interpreter thread.
class PythonGatewayActor final : public EventBasedActor {
  public:
    /// \brief Actor type name constant used by the registry.
    static constexpr std::string_view kActorTypeName{"hpactor.python.gateway"};

    /// \brief Construct the gateway actor.
    ///
    /// \param[in] context Actor context (always nullptr during construction).
    /// \param[in] system The owning actor system.
    /// \param[in] runtime The Python bridge runtime that owns the command
    /// queue.
    /// \param[in] executor Fixed function-pointer port for command execution.
    PythonGatewayActor(ActorContext* context, ActorSystem& system,
                       PythonRuntime& runtime,
                       PythonCommandExecutorPort executor) noexcept;

    /// \brief Process an incoming typed message.
    ///
    /// Intercepts \c kPythonWakeupTag before the base system-tag switch and
    /// delegates to \c handle_wakeup(). All other tags are forwarded to
    /// \c EventBasedActor::receive().
    ///
    /// \param[in,out] message The incoming message to process.
    void receive(TypedMessage& message) override;

    /// \brief Return the actor type name for metrics and introspection.
    ///
    /// \return The \c kActorTypeName constant.
    std::string_view type_name() const noexcept override {
        return kActorTypeName;
    }

  private:
    /// \brief Handle a wakeup signal by draining and executing commands.
    ///
    /// Drains at most \c max_commands_per_turn commands from the runtime's
    /// command queue, executes each through the executor port, and pushes
    /// requested completions. If commands remain, self-sends an empty
    /// \c kPythonWakeupTag message to requeue.
    ///
    /// \param[in] message The wakeup message (payload is ignored).
    void handle_wakeup(TypedMessage& message) noexcept;

    PythonRuntime& runtime_;
    PythonCommandExecutorPort executor_;
};

} // namespace hpactor::python
