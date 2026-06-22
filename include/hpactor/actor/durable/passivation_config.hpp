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

#include <hpactor/actor/durable/recovery_policy.hpp>

#include <chrono>
#include <cstdint>

namespace hpactor::actor::durable {

/// \brief Millisecond-precision duration alias for passivation timeouts.
using Duration = std::chrono::milliseconds;

/// \brief Per-actor configuration for passivation behavior.
///
/// Controls idle timeout, schema versioning, snapshot preferences, and
/// recovery policy. Instances are provided to \c PassivationManager at
/// actor registration and stored for the actor's lifetime.
struct PassivationConfig {
    /// \brief Idle duration after which the actor is eligible for
    ///        passivation. Default: 30 minutes.
    Duration idle_timeout = std::chrono::minutes(30);

    /// \brief Schema version of the actor's state. Compared against stored
    ///        snapshots during recovery; a mismatch triggers
    ///        \c IDurableActor::migrate_snapshot().
    uint32_t schema_version = 1;

    /// \brief Recovery strategy when event replay or snapshot restoration
    ///        fails.
    RecoveryPolicy recovery_policy = RecoveryPolicy::FailActor;

    /// \brief Take a snapshot before passivating the actor.
    bool snapshot_on_passivate = true;

    /// \brief Take a snapshot during graceful shutdown.
    bool snapshot_on_shutdown = true;
};

} // namespace hpactor::actor::durable
