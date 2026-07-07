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

#include <hpactor/python/python_gateway_actor.hpp>
#include <hpactor/python/python_ports.hpp>
#include <hpactor/ref/actor_address.hpp>

namespace hpactor {

class ActorSystem;

} // namespace hpactor

namespace hpactor::python {

/// \brief Adapter that bridges the runtime's \c GatewayWakePort to the
///        native gateway actor via \c kPythonWakeupTag delivery.
///
/// Owns stable references to the \c ActorSystem and the gateway actor's
/// address. The static \c wake() function is registered as the runtime's
/// wake callback and constructs a single empty \c kPythonWakeupTag message
/// that is delivered to the gateway through
/// \c ActorSystem::deliver_with_result().
///
/// \note Thread safety: \c wake() is called from the Python interpreter
///       thread (the command producer). \c deliver_with_result() is safe
///       to call from any thread.
class PythonGatewayWakeAdapter final {
  public:
    /// \brief Construct the wake adapter.
    ///
    /// \param[in] system The owning actor system.
    /// \param[in] gateway The address of the PythonGatewayActor to wake.
    PythonGatewayWakeAdapter(ActorSystem& system, ActorAddress gateway) noexcept;

    /// \brief Return a \c GatewayWakePort suitable for registering with the
    ///        runtime.
    ///
    /// \return A valid \c GatewayWakePort with \c wake() as the callback.
    [[nodiscard]] GatewayWakePort port() noexcept;

  private:
    /// \brief Static wake callback invoked by the runtime when a command is
    ///        enqueued.
    ///
    /// Constructs an empty \c kPythonWakeupTag message and delivers it to the
    /// gateway actor via \c deliver_with_result().
    ///
    /// \param[in] context Pointer to the owning \c PythonGatewayWakeAdapter.
    /// \return true if the wake message was accepted for delivery.
    static bool wake(void* context) noexcept;

    ActorSystem& system_;
    ActorAddress gateway_;
};

} // namespace hpactor::python
