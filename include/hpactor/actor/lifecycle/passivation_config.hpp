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

#include <chrono>
#include <cstdint>

namespace hpactor {

/// \brief Per-actor passivation configuration.
struct PassivationConfig {
    /// \brief Idle duration before automatic passivation (0 = disabled).
    std::chrono::milliseconds idle_timeout{0};

    /// \brief Whether passivation persists state via DurableStateStore.
    bool durable = false;

    /// \brief Whether memory-pressure monitor may select this actor.
    bool allow_memory_pressure = true;

    /// \brief Schema version for durable snapshot compatibility.
    uint32_t schema_version = 1;
};

/// \brief Metadata recorded when an actor enters kPassivated.
struct PassivationRecord {
    std::chrono::steady_clock::time_point passivated_at{};

    /// \brief Snapshot sequence assigned by DurableStateStore (0 if none).
    uint64_t snapshot_sequence = 0;

    uint32_t schema_version = 1;

    /// \brief What triggered this passivation.
    enum class Trigger : uint8_t {
        kIdle = 0,           ///< Idle timeout expired.
        kSelf = 1,           ///< Actor called context()->passivate().
        kMemoryPressure = 2, ///< Memory pressure monitor selected this actor.
        kCli = 3,            ///< Operator issued /actor passivate.
    };
    Trigger trigger = Trigger::kIdle;
};

} // namespace hpactor
