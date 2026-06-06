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
#include <hpactor/types/types.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace hpactor {

/// \brief A persisted snapshot record.
struct SnapshotRecord {
    std::string persistence_id;
    uint64_t sequence = 0;
    uint32_t schema_version = 1;
    uint32_t serializer_id = 0;
    uint64_t timestamp_ms = 0;
    StreamBuffer data;
    uint32_t checksum = 0;
};

/// \brief A persisted event record (for event-sourced actors).
struct EventRecord {
    std::string persistence_id;
    uint64_t sequence = 0;
    uint32_t schema_version = 1;
    uint32_t serializer_id = 0;
    uint64_t timestamp_ms = 0;
    StreamBuffer event_data;
};

/// \brief Abstract persistence backend for durable actor state.
class DurableStateStore {
  public:
    virtual ~DurableStateStore() = default;

    virtual result<SnapshotRecord>
    write_snapshot(std::string_view persistence_id, uint32_t schema_version,
                   StreamBuffer data) = 0;

    virtual result<SnapshotRecord>
    load_latest_snapshot(std::string_view persistence_id) = 0;

    virtual result<void> append_event(std::string_view persistence_id,
                                      uint64_t sequence, StreamBuffer event) = 0;

    virtual result<std::vector<EventRecord>>
    load_events_after(std::string_view persistence_id, uint64_t after_sequence) = 0;

    virtual result<void> delete_state(std::string_view persistence_id) = 0;

    virtual std::string_view store_type() const = 0;
};

} // namespace hpactor
