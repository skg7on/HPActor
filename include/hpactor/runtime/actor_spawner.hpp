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

#include <hpactor/actor/system/actor_directory.hpp>
#include <hpactor/actor/system/actor_system.hpp>
#include <hpactor/runtime/actor_spawn_lease.hpp>
#include <hpactor/types/types.hpp>

#include <hpactor/runtime/spawn_spec.hpp>

#include <memory>

namespace hpactor {

class AbstractActor;
class ActorContext;

namespace sched {
class IScheduler;
}

/// \brief Sole publisher of complete actor directory entries.
///
/// Stores only stable non-owning references.  All owners outlive it.
/// Performs no topology lookup, delivery, remote I/O, or passivation.
///
/// \note Thread safety: Must be externally serialized. The spawner publishes
///       into a shared \c ActorDirectory — concurrent adoptions would race
///       on directory insertion.
class ActorSpawner final {
  public:
    /// \brief Non-owning dependencies that must outlive the spawner.
    struct Dependencies {
        ActorSystem& facade;          ///< Owning actor system facade.
        EndPoint endpoint;            ///< Local endpoint.
        ActorDirectory& directory;    ///< Target directory for publication.
        sched::IScheduler& scheduler; ///< Scheduler for worker assignment.
        metrics::MpscRingBuffer<metrics::MetricEvent>* metrics; ///< Metrics
                                                                ///< ring buffer
                                                                ///< (nullable).
        log::Logger* logger; ///< Structured logger (nullable).
    };

    /// \brief Construct the spawner with fixed dependencies.
    /// \param[in] dependencies Stable non-owning references.
    explicit ActorSpawner(Dependencies dependencies) noexcept;

    /// \brief Adopt a constructed local actor through the unified pipeline.
    ///
    /// Returns a valid \c Actor on success, or a typed error on failure.
    /// The spawner is the only production code that constructs and publishes
    /// a complete \c ActorDirectoryEntry.
    ///
    /// \param[in] actor The fully constructed actor (shared ownership
    ///                  transferred to the directory).
    /// \param[in] spec  Resolved spawn specification with effective config.
    /// \return A valid \c Actor on success, or a typed error code.
    result<Actor>
    adopt(std::shared_ptr<AbstractActor> actor, const SpawnSpec& spec) noexcept;

    /// \brief Adopt a constructed actor without publishing a name.
    ///
    /// Performs the adoption pipeline up to directory publication, but
    /// publishes with no registered name. Returns an \c ActorSpawnLease
    /// that can be committed (transferring ownership) or rolled back
    /// (removing the actor from the directory).
    ///
    /// \param[in] actor The fully constructed actor.
    /// \param[in] spec  Resolved spawn specification.
    /// \return An \c ActorSpawnLease on success, or a typed error code.
    result<ActorSpawnLease>
    adopt_unpublished(std::shared_ptr<AbstractActor> actor,
                      const SpawnSpec& spec) noexcept;

  private:
    void rollback_publication(ActorId id, AbstractActor& actor) noexcept;

    Dependencies deps_;
};

} // namespace hpactor
