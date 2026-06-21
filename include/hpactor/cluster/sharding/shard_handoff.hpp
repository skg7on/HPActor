// Copyright 2026 HPActor Contributors
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

#include <cstdint>
#include <string>

namespace hpactor::cluster::sharding {

/// \brief Handoff state for a single shard during ownership transfer.
enum class HandoffState : uint8_t {
    Owned,        ///< Normal operation; this node owns the shard.
    Draining,     ///< Stopping new user messages; draining in-flight.
    Transferring, ///< Handing off to new owner.
    Recovering,   ///< New owner rebuilding state.
    Active,       ///< New owner fully operational.
};

/// \brief Tracks handoff lifecycle for a single shard.
class ShardHandoff {
  public:
    explicit ShardHandoff(uint32_t shard_id);

    uint32_t shard_id() const;
    HandoffState state() const;
    const std::string& new_owner() const;

    /// \brief Transition Owned → Draining.
    bool begin_drain();

    /// \brief Transition Draining → Transferring.
    bool complete_drain();

    /// \brief Transition Transferring → Recovering.
    bool begin_recovery(const std::string& new_owner_node);

    /// \brief Transition Recovering → Active.
    bool activate();

    /// \brief Abort from Draining back to Owned.
    bool abort();

  private:
    uint32_t shard_id_;
    HandoffState state_ = HandoffState::Owned;
    std::string new_owner_;
};

} // namespace hpactor::cluster::sharding
