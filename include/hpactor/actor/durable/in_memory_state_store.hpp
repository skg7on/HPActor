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

#include <hpactor/actor/durable_state_store.hpp>

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace hpactor {

/// \brief In-memory DurableStateStore for tests and single-node use.
class InMemoryStateStore : public DurableStateStore {
  public:
    InMemoryStateStore() = default;

    result<SnapshotRecord>
    write_snapshot(std::string_view persistence_id, uint32_t schema_version,
                   StreamBuffer data) override;

    result<SnapshotRecord>
    load_latest_snapshot(std::string_view persistence_id) override;

    result<void> append_event(std::string_view persistence_id,
                              uint64_t sequence, StreamBuffer event) override;

    result<std::vector<EventRecord>>
    load_events_after(std::string_view persistence_id,
                      uint64_t after_sequence) override;

    result<void> delete_state(std::string_view persistence_id) override;

    std::string_view store_type() const override {
        return "in_memory";
    }

  private:
    struct ActorState {
        uint64_t next_sequence = 0;
        SnapshotRecord latest_snapshot;
        std::vector<EventRecord> events;
    };

    mutable std::mutex mutex_;
    std::unordered_map<std::string, ActorState> states_;
};

} // namespace hpactor
