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

#include <hpactor/actor/durable/durable_behavior.hpp>
#include <hpactor/actor/durable/durable_state_store.hpp>
#include <hpactor/msg/failure_reason.hpp>
#include <hpactor/types/types.hpp>

#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace hpactor::actor::durable {

/// \brief Simple test state for durable behavior unit tests.
struct TestState {
    int counter = 0;
    std::string name;
};

/// \brief Simple test event for event-sourced behavior unit tests.
struct TestEvent {
    int delta = 0;
};

/// \brief In-memory DurableStateStore for durable behavior tests.
///
/// Simpler than the production \c InMemoryStateStore — no mutex, no
/// sequence validation, no timestamp. Suitable for single-threaded
/// unit tests of \c DurableBehavior and \c EventSourcedBehavior.
class TestInMemoryStore : public hpactor::DurableStateStore {
  public:
    hpactor::result<hpactor::SnapshotRecord>
    write_snapshot(std::string_view pid, uint32_t schema,
                   hpactor::StreamBuffer data) override {
        auto& next_seq = next_seq_[std::string(pid)];
        hpactor::SnapshotRecord rec;
        rec.persistence_id = std::string(pid);
        rec.sequence = ++next_seq;
        rec.schema_version = schema;
        rec.data = std::move(data);
        records_[std::string(pid)] = rec;
        return hpactor::result<hpactor::SnapshotRecord>::make(
            hpactor::SnapshotRecord(rec));
    }

    hpactor::result<hpactor::SnapshotRecord>
    load_latest_snapshot(std::string_view pid) override {
        auto it = records_.find(std::string(pid));
        if (it == records_.end())
            return hpactor::result<hpactor::SnapshotRecord>::make(hpactor::error(
                static_cast<uint32_t>(hpactor::FailureReason::NoRoute)));
        return hpactor::result<hpactor::SnapshotRecord>::make(
            hpactor::SnapshotRecord(it->second));
    }

    hpactor::result<void> append_event(std::string_view pid, uint64_t seq,
                                       hpactor::StreamBuffer event) override {
        auto& next_seq = next_seq_[std::string(pid)];
        // Accept the caller-supplied sequence, but track the high-water mark
        if (seq > next_seq)
            next_seq = seq;
        hpactor::EventRecord rec;
        rec.persistence_id = std::string(pid);
        rec.sequence = seq;
        rec.schema_version = 1;
        rec.event_data = std::move(event);
        events_[std::string(pid)].push_back(std::move(rec));
        return hpactor::result<void>::make();
    }

    hpactor::result<std::vector<hpactor::EventRecord>>
    load_events_after(std::string_view pid, uint64_t after) override {
        std::vector<hpactor::EventRecord> result;
        auto it = events_.find(std::string(pid));
        if (it != events_.end()) {
            for (const auto& ev : it->second) {
                if (ev.sequence > after)
                    result.push_back(ev);
            }
        }
        return hpactor::result<std::vector<hpactor::EventRecord>>::make(
            std::move(result));
    }

    hpactor::result<void> delete_state(std::string_view pid) override {
        records_.erase(std::string(pid));
        events_.erase(std::string(pid));
        next_seq_.erase(std::string(pid));
        return hpactor::result<void>::make();
    }

    std::string_view store_type() const override {
        return "test";
    }

  private:
    std::unordered_map<std::string, hpactor::SnapshotRecord> records_;
    std::unordered_map<std::string, std::vector<hpactor::EventRecord>> events_;
    std::unordered_map<std::string, uint64_t> next_seq_;
};

// ---- Shared serialization specializations for TestState ----
// Defined here so both test_durable_behavior and test_event_sourced_behavior
// can share them without duplicate-symbol linker errors.

template <> inline StreamBuffer serialize_state(const TestState& s) {
    std::string data = std::to_string(s.counter) + "|" + s.name;
    return StreamBuffer::from_data(reinterpret_cast<const uint8_t*>(data.c_str()),
                                   data.size());
}

template <>
inline result<TestState> deserialize_state(const StreamBuffer& data) {
    std::string s(reinterpret_cast<const char*>(data.data()), data.size());
    auto pipe = s.find('|');
    if (pipe == std::string::npos)
        return result<TestState>::make(
            error(static_cast<uint32_t>(FailureReason::ReactivationFailed)));
    TestState st;
    st.counter = std::stoi(s.substr(0, pipe));
    st.name = s.substr(pipe + 1);
    return result<TestState>::make(std::move(st));
}

} // namespace hpactor::actor::durable
