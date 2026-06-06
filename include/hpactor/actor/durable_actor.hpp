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

#include <hpactor/adt/stream_buffer.hpp>
#include <hpactor/msg/failure_reason.hpp>
#include <hpactor/types/types.hpp>

#include <string_view>

namespace hpactor {

/// \brief Opt-in interface for actors whose state survives passivation
///        across process restarts.
class IDurableActor {
  public:
    virtual ~IDurableActor() = default;

    /// \brief Stable identity across passivation and restart cycles.
    virtual std::string_view persistence_id() const = 0;

    /// \brief Serialize current in-memory state for a snapshot.
    virtual result<StreamBuffer> snapshot_state() const = 0;

    /// \brief Restore in-memory state from a previously persisted snapshot.
    virtual result<void> restore_snapshot(const StreamBuffer& data) = 0;

    /// \brief Apply a persisted event to in-memory state (event-sourced
    /// actors).
    virtual result<void> apply_event(const StreamBuffer& /*event*/) {
        return result<void>::make();
    }

    /// \brief Migrate a snapshot from an older schema version.
    virtual result<StreamBuffer>
    migrate_snapshot(uint32_t /*from_version*/, const StreamBuffer& /*data*/) {
        return result<StreamBuffer>::make(
            error(static_cast<uint32_t>(FailureReason::SchemaVersionMismatch)));
    }
};

} // namespace hpactor
