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

#include <hpactor/actor/durable/passivation_config.hpp>
#include <hpactor/types/types.hpp>

#include <cstddef>
#include <mutex>
#include <string>
#include <unordered_map>

namespace hpactor::actor::durable {

/// \brief Tracks the passivation lifecycle state of a durable actor.
enum class PassivationState : uint8_t {
    /// Actor is active and accepting messages.
    Active,
    /// Passivation has been requested; actor is draining.
    Passivating,
    /// Actor has been passivated (state persisted, actor removed).
    Passivated,
};

/// \brief Manages durable actor passivation lifecycle.
///
/// Tracks registered durable actors and their passivation state transitions:
/// Active -> Passivating -> Passivated (removed). Thread-safe — all public
/// methods acquire an internal mutex.
///
/// Owned by \c ActorSystem (one per node). When an actor is passivated,
/// its state is persisted before unregistration. Reactivation creates a
/// fresh actor instance that recovers state from the store.
class PassivationManager {
  public:
    PassivationManager() = default;

    /// \brief Register a durable actor for passivation tracking.
    ///
    /// \param[in] actor_id        The actor's runtime identifier.
    /// \param[in] persistence_id  Stable identity used for store lookups.
    /// \param[in] config          Passivation settings (idle timeout, schema,
    ///                            recovery policy, snapshot preferences).
    void register_actor(ActorId actor_id, const std::string& persistence_id,
                        const PassivationConfig& config);

    /// \brief Remove an actor from passivation tracking.
    ///
    /// Called after successful passivation or during actor termination.
    void unregister_actor(ActorId actor_id);

    /// \brief Check whether an actor is currently tracked.
    ///
    /// \return true if the actor is registered (in any state).
    bool is_tracked(ActorId actor_id) const;

    /// \brief Begin passivation for an actor.
    ///
    /// Transitions the actor from \c Active to \c Passivating.
    ///
    /// \return true if the transition was successful, false if the actor
    ///         is not tracked or is not in \c Active state.
    bool begin_passivate(ActorId actor_id);

    /// \brief Complete passivation, removing the actor from tracking.
    ///
    /// Called after state has been successfully persisted to the store.
    void complete_passivation(ActorId actor_id);

    /// \brief Get the current passivation state of an actor.
    ///
    /// \return \c Passivated if the actor is not tracked.
    PassivationState get_state(ActorId actor_id) const;

    /// \brief Number of currently tracked actors.
    size_t tracked_count() const;

  private:
    struct ActorRecord {
        std::string persistence_id;
        PassivationConfig config;
        PassivationState state = PassivationState::Active;
    };

    std::unordered_map<uint64_t, ActorRecord> actors_;
    mutable std::mutex mutex_;
};

} // namespace hpactor::actor::durable
