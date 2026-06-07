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

#include <hpactor/actor/durable/in_memory_state_store.hpp>

#include <chrono>

namespace hpactor {

result<SnapshotRecord>
InMemoryStateStore::write_snapshot(std::string_view persistence_id,
                                   uint32_t schema_version, StreamBuffer data) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string key(persistence_id);
    auto& state = states_[key];
    uint64_t seq = state.next_sequence++;

    SnapshotRecord rec;
    rec.persistence_id = key;
    rec.sequence = seq;
    rec.schema_version = schema_version;
    rec.serializer_id = 0;
    rec.timestamp_ms = static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    rec.data = std::move(data);
    rec.checksum = 0;
    state.latest_snapshot = rec;
    return result<SnapshotRecord>::make(std::move(rec));
}

result<SnapshotRecord>
InMemoryStateStore::load_latest_snapshot(std::string_view persistence_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = states_.find(std::string(persistence_id));
    if (it == states_.end() || it->second.latest_snapshot.persistence_id.empty()) {
        return result<SnapshotRecord>::make(
            error(static_cast<uint32_t>(FailureReason::Unknown)));
    }
    return result<SnapshotRecord>::make(SnapshotRecord(it->second.latest_snapshot));
}

result<void>
InMemoryStateStore::append_event(std::string_view persistence_id,
                                 uint64_t sequence, StreamBuffer event) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string key(persistence_id);
    auto& state = states_[key];
    if (state.next_sequence > sequence) {
        return result<void>::make();
    }
    if (state.next_sequence != sequence) {
        return result<void>::make(
            error(static_cast<uint32_t>(FailureReason::Unknown)));
    }

    EventRecord rec;
    rec.persistence_id = key;
    rec.sequence = state.next_sequence++;
    rec.schema_version = 1;
    rec.serializer_id = 0;
    rec.timestamp_ms = static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    rec.event_data = std::move(event);
    state.events.push_back(std::move(rec));
    return result<void>::make();
}

result<std::vector<EventRecord>>
InMemoryStateStore::load_events_after(std::string_view persistence_id,
                                      uint64_t after_sequence) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = states_.find(std::string(persistence_id));
    if (it == states_.end()) {
        std::vector<EventRecord> empty;
        return result<std::vector<EventRecord>>::make(std::move(empty));
    }
    std::vector<EventRecord> events;
    for (const auto& ev : it->second.events) {
        if (ev.sequence > after_sequence) {
            events.push_back(ev);
        }
    }
    return result<std::vector<EventRecord>>::make(std::move(events));
}

result<void> InMemoryStateStore::delete_state(std::string_view persistence_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    states_.erase(std::string(persistence_id));
    return result<void>::make();
}

} // namespace hpactor
