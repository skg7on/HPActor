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

#include <hpactor/actor/durable/file_state_store.hpp>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace hpactor {

namespace fs = std::filesystem;

FileStateStore::FileStateStore(std::string root_dir)
    : root_dir_(std::move(root_dir)) {
    fs::create_directories(root_dir_);
}

std::string FileStateStore::actor_dir(std::string_view persistence_id) const {
    return (fs::path(root_dir_) / std::string(persistence_id)).string();
}

uint32_t FileStateStore::crc32c(const uint8_t* data, size_t len) {
    uint32_t crc = 0;
    for (size_t i = 0; i < len; ++i) {
        crc = crc ^ data[i];
        for (int j = 0; j < 8; ++j) {
            crc = (crc >> 1) ^ (0x82F63B78 & -(crc & 1));
        }
    }
    return crc;
}

result<SnapshotRecord>
FileStateStore::write_snapshot(std::string_view persistence_id,
                               uint32_t schema_version, StreamBuffer data) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string key(persistence_id);
    std::string dir = actor_dir(persistence_id);
    fs::create_directories(dir);

    uint64_t seq = next_sequences_[key]++;

    SnapshotRecord rec;
    rec.persistence_id = key;
    rec.sequence = seq;
    rec.schema_version = schema_version;
    rec.serializer_id = 0;
    rec.timestamp_ms = static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    rec.data = data;
    rec.checksum = crc32c(data.data(), data.size());

    std::string tmp_path = dir + "/snapshot.tmp";
    std::string final_path = dir + "/snapshot.bin";
    {
        std::ofstream ofs(tmp_path, std::ios::binary | std::ios::trunc);
        if (!ofs) {
            return result<SnapshotRecord>::make(error(
                static_cast<uint32_t>(FailureReason::PassivationSnapshotFailed)));
        }
        auto write_le = [&](uint64_t val) {
            ofs.write(reinterpret_cast<const char*>(&val), sizeof(val));
        };
        write_le(rec.sequence);
        write_le(rec.schema_version);
        write_le(rec.checksum);
        write_le(data.size());
        ofs.write(reinterpret_cast<const char*>(data.data()),
                  static_cast<std::streamsize>(data.size()));
        if (!ofs) {
            fs::remove(tmp_path);
            return result<SnapshotRecord>::make(error(
                static_cast<uint32_t>(FailureReason::PassivationSnapshotFailed)));
        }
    }
    fs::rename(tmp_path, final_path);
    return result<SnapshotRecord>::make(std::move(rec));
}

result<SnapshotRecord>
FileStateStore::load_latest_snapshot(std::string_view persistence_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string final_path = actor_dir(persistence_id) + "/snapshot.bin";
    if (!fs::exists(final_path)) {
        return result<SnapshotRecord>::make(
            error(static_cast<uint32_t>(FailureReason::Unknown)));
    }

    std::ifstream ifs(final_path, std::ios::binary);
    if (!ifs) {
        return result<SnapshotRecord>::make(
            error(static_cast<uint32_t>(FailureReason::ReactivationFailed)));
    }

    SnapshotRecord rec;
    rec.persistence_id = std::string(persistence_id);

    auto read_le = [&]() -> uint64_t {
        uint64_t v = 0;
        ifs.read(reinterpret_cast<char*>(&v), sizeof(v));
        return v;
    };

    rec.sequence = read_le();
    rec.schema_version = static_cast<uint32_t>(read_le());
    rec.checksum = static_cast<uint32_t>(read_le());
    uint64_t data_len = read_le();

    StreamBuffer data(data_len);
    ifs.read(reinterpret_cast<char*>(data.data()),
             static_cast<std::streamsize>(data_len));
    if (!ifs) {
        return result<SnapshotRecord>::make(
            error(static_cast<uint32_t>(FailureReason::ReactivationFailed)));
    }

    uint32_t computed = crc32c(data.data(), data.size());
    if (computed != rec.checksum) {
        return result<SnapshotRecord>::make(
            error(static_cast<uint32_t>(FailureReason::SchemaVersionMismatch)));
    }

    rec.data = std::move(data);
    return result<SnapshotRecord>::make(std::move(rec));
}

result<void>
FileStateStore::append_event(std::string_view persistence_id, uint64_t sequence,
                             StreamBuffer /*event*/) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string key(persistence_id);
    if (next_sequences_[key] > sequence) {
        return result<void>::make();
    }
    if (next_sequences_[key] != sequence) {
        return result<void>::make(
            error(static_cast<uint32_t>(FailureReason::Unknown)));
    }
    next_sequences_[key]++;
    return result<void>::make();
}

result<std::vector<EventRecord>>
FileStateStore::load_events_after(std::string_view /*persistence_id*/,
                                  uint64_t /*after_sequence*/) {
    std::vector<EventRecord> empty;
    return result<std::vector<EventRecord>>::make(std::move(empty));
}

result<void> FileStateStore::delete_state(std::string_view persistence_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string key(persistence_id);
    next_sequences_.erase(key);
    std::string dir = actor_dir(persistence_id);
    std::error_code ec;
    fs::remove_all(dir, ec);
    return ec ? result<void>::make(
                    error(static_cast<uint32_t>(FailureReason::Unknown)))
              : result<void>::make();
}

} // namespace hpactor
