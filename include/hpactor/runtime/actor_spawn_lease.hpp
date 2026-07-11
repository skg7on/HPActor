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

#include <hpactor/actor/system/actor_system.hpp>
#include <hpactor/types/types.hpp>

#include <cstdint>
#include <memory>

namespace hpactor {

class AbstractActor;

/// \brief A move-only ownership lease for an internally unpublished actor.
///
/// Produced by \c ActorSpawner::adopt_unpublished(). The actor is inserted
/// into the directory (with no registered name) and is internally reachable
/// by ID, but it has no external name and is not yet part of the committed
/// topology.
///
/// \c commit() transfers ownership to the runtime and disables destructor
/// rollback. \c rollback() reverses completed adoption steps in reverse
/// order and is idempotent. The destructor calls \c rollback() if the lease
/// has not been committed.
class ActorSpawnLease final {
  public:
    /// \brief Default-construct an empty (already-rolled-back) lease.
    ActorSpawnLease() = default;

    /// \brief Move-construct. The source lease becomes empty.
    ActorSpawnLease(ActorSpawnLease&& other) noexcept;

    /// \brief Move-assign. The previous lease (if any) is rolled back first.
    ActorSpawnLease& operator=(ActorSpawnLease&& other) noexcept;

    /// \brief Destroy the lease. Calls \c rollback() if not committed.
    ~ActorSpawnLease();

    ActorSpawnLease(const ActorSpawnLease&) = delete;
    ActorSpawnLease& operator=(const ActorSpawnLease&) = delete;

    /// \brief The actor handle for the internally published actor.
    [[nodiscard]] const Actor& actor() const noexcept;

    /// \brief Transfer ownership to the runtime. After this call, rollback
    ///        is a no-op and the destructor does nothing.
    ///
    /// \return ok() on success.
    [[nodiscard]] result<void> commit() noexcept;

    /// \brief Reverse all completed adoption steps. Idempotent.
    ///
    /// Removes the directory entry, unregisters scheduler placement, and
    /// runs actor cleanup. Safe to call multiple times.
    ///
    /// \return ok() on success.
    [[nodiscard]] result<void> rollback() noexcept;

    /// \brief Whether the lease has been committed and no longer owns the actor.
    [[nodiscard]] bool committed() const noexcept;

    /// \brief Whether the lease is empty (default-constructed or moved-from).
    [[nodiscard]] bool empty() const noexcept;

  private:
    friend class ActorSpawner;

    /// \brief Construct a lease from an adopted actor.
    ///
    /// \param[in] actor_handle  The actor handle from the directory.
    /// \param[in] actor_id      The actor's ID.
    /// \param[in] directory     Non-owning pointer to the directory.
    /// \param[in] scheduler     Non-owning pointer to the scheduler.
    ActorSpawnLease(Actor actor_handle, ActorId actor_id,
                    ActorDirectory* directory,
                    sched::IScheduler* scheduler) noexcept;

    Actor actor_handle_;
    ActorId actor_id_{};
    ActorDirectory* directory_{nullptr};
    sched::IScheduler* scheduler_{nullptr};
    bool committed_{false};
};

} // namespace hpactor
