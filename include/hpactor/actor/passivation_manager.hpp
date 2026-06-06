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

#include <hpactor/actor/passivation_config.hpp>
#include <hpactor/types/types.hpp>

#include <memory>

namespace hpactor {

class ActorSystem;
class DurableStateStore;
class IActorRoute;
class LocalActor;

/// \brief Orchestrates the passivation and reactivation protocols.
///
/// Coordinates drain, snapshot persistence, route stub installation,
/// and lazy reactivation. Owned by ActorSystem.
class PassivationManager {
  public:
    /// \brief Construct with system references.
    PassivationManager(ActorSystem& system, DurableStateStore* durable_store,
                       PassivationConfig default_config);

    ~PassivationManager();

    PassivationManager(const PassivationManager&) = delete;
    PassivationManager& operator=(const PassivationManager&) = delete;

    /// \brief Begin passivation for an actor.
    ///
    /// Transitions Active → Passivating, drains the mailbox,
    /// persists state (durable or memory-only), and installs a
    /// LocalPassivatedRoute in place of the live actor.
    ///
    /// \param[in] actor_id The actor to passivate.
    /// \param[in] trigger   What initiated passivation.
    /// \return true if passivation started successfully.
    bool begin_passivation(ActorId actor_id, PassivationRecord::Trigger trigger);

    /// \brief Handle actor self-passivation request.
    ///
    /// Called after the current handler returns when
    /// ActorContext::passivate() was invoked.
    ///
    /// \param[in] actor The actor requesting passivation.
    void handle_self_passivation(LocalActor& actor);

    /// \brief Reactivate a passivated actor.
    ///
    /// Restores state from DurableStateStore or HibernationRegistry,
    /// constructs a new actor instance, transitions Recovering → Active,
    /// and replaces the route stub in the registry.
    ///
    /// \param[in] route The passivated route stub.
    /// \return The reactivated actor, or an error.
    result<LocalActor*> reactivate(IActorRoute& route);

    /// \brief System-level passivation defaults.
    const PassivationConfig& default_config() const noexcept {
        return default_config_;
    }

    /// \brief Access the durable store (may be null if no store configured).
    DurableStateStore* durable_store() const noexcept {
        return durable_store_;
    }

  private:
    result<void> drain_actor(ActorId actor_id);
    result<void> persist_and_release(ActorId actor_id, PassivationRecord record);

    ActorSystem& system_;
    DurableStateStore* durable_store_;
    PassivationConfig default_config_;
};

} // namespace hpactor
