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

#include <hpactor/actor/actor_directory.hpp>
#include <hpactor/actor/actor_system.hpp>
#include <hpactor/types/types.hpp>

#include "spawn_spec.hpp"

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
class ActorSpawner final {
  public:
    struct Dependencies {
        ActorSystem& facade;
        EndPoint endpoint;
        ActorDirectory& directory;
        sched::IScheduler& scheduler;
        metrics::MpscRingBuffer<metrics::MetricEvent>* metrics;
        log::Logger* logger;
    };

    explicit ActorSpawner(Dependencies dependencies) noexcept;

    /// \brief Adopt a constructed local actor through the unified pipeline.
    ///
    /// Returns a valid \c Actor on success, or a typed error on failure.
    /// The spawner is the only production code that constructs and publishes
    /// a complete \c ActorDirectoryEntry.
    result<Actor>
    adopt(std::shared_ptr<AbstractActor> actor, const SpawnSpec& spec) noexcept;

  private:
    void rollback_publication(ActorId id, AbstractActor& actor) noexcept;

    Dependencies deps_;
};

} // namespace hpactor
