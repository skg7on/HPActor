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
#include <hpactor/python/python_runtime.hpp>

#include <cstdint>
#include <string_view>

namespace hpactor::python {

/// \brief Native C++ bridge actor that serves as the HPActor-side identity for
///        each Python-bound actor.
///
/// Each PythonBridgeActor holds a reserved \c PythonActorLease that binds its
/// \c ActorId to a monotonic generation in the \c PythonRuntime registry.
/// Incoming \c TypedMessage envelopes are converted to \c
/// PythonDispatchEnvelope objects and pushed into the bounded dispatch queue
/// for consumption by the Python event loop.
///
/// System messages (TypeTag < TypeTag::User) are forwarded to
/// \c EventBasedActor::receive() for normal handling. User messages go through
/// the drain and lifecycle gates, then are transferred to the Python side with
/// full delivery metadata (sender, message ID, priority, deadline, trace
/// context). Reliable messages trigger ACK (status 0) on accepted transfer or
/// NACK (status 1, 500 ms retry hint) on rejection.
///
/// \note Thread affinity: runs on the scheduler like any EventBasedActor.
///       Queue push is lock-free; the Python event loop drains independently.
class PythonBridgeActor final : public EventBasedActor {
  public:
    /// \brief Actor type name constant used by the registry.
    static constexpr std::string_view kActorTypeName{"hpactor.python.bridge"};

    /// \brief Construct the bridge actor.
    ///
    /// \param[in] context Actor context (always nullptr during construction).
    /// \param[in] system The owning actor system.
    /// \param[in] runtime The Python bridge runtime that owns the dispatch
    /// queue.
    /// \param[in] lease A reserved actor lease to bind on activation.
    PythonBridgeActor(ActorContext* context, ActorSystem& system,
                      PythonRuntime& runtime, PythonActorLease lease) noexcept;

    /// \brief Process an incoming typed message.
    ///
    /// System messages (tag < TypeTag::User) are delegated to the base class.
    /// User messages go through the drain/lifecycle gates, are converted to a
    /// \c PythonDispatchEnvelope, and pushed into the runtime dispatch queue.
    /// Reliable messages receive ACK or NACK based on transfer success.
    ///
    /// \param[in,out] message The incoming message to process or transfer.
    void receive(TypedMessage& message) override;

    /// \brief Activate the bridge actor and bind the reserved lease.
    ///
    /// Calls \c EventBasedActor::on_activate() first, then binds the reserved
    /// \c PythonActorLease to \c id().
    void on_activate() override;

    /// \brief Deactivate the bridge actor and release the lease.
    ///
    /// Resets the \c PythonActorLease before calling
    /// \c EventBasedActor::on_deactivate().
    void on_deactivate() override;

    /// \brief Return the monotonic generation assigned to this actor.
    ///
    /// \return The generation value from the bound lease, or 0 if not yet
    /// bound.
    [[nodiscard]] uint64_t generation() const noexcept;

    /// \brief Return the actor type name for metrics and introspection.
    ///
    /// \return The \c kActorTypeName constant.
    std::string_view type_name() const noexcept override {
        return kActorTypeName;
    }

  private:
    PythonRuntime& runtime_;
    PythonActorLease lease_;
    uint64_t next_dispatch_sequence_{1};
};

} // namespace hpactor::python
