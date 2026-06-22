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

#include <hpactor/mailbox/file_delivery_store.hpp>
#include <hpactor/msg/failure_reason.hpp>

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace hpactor::mailbox {

using msg::PendingSend;

FileDeliveryStore::FileDeliveryStore(std::string root_dir)
    : root_dir_(std::move(root_dir)) {
    std::filesystem::create_directories(root_dir_);
}

std::string FileDeliveryStore::outbox_path() const {
    return root_dir_ + "/outbox.dat";
}

std::string FileDeliveryStore::inbox_path() const {
    return root_dir_ + "/inbox.dat";
}

result<void> FileDeliveryStore::put_outbox(const PendingSend& record) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string path = outbox_path();
    std::string tmp = path + ".tmp";

    // Copy existing entries into temp file to preserve prior records,
    // then append the new entry. Atomic rename guarantees consistency.
    if (std::filesystem::exists(path)) {
        std::ifstream ifs(path);
        if (ifs) {
            std::ofstream ofs(tmp);
            if (!ofs) {
                return result<void>::make(error(static_cast<uint32_t>(
                    FailureReason::PassivationSnapshotFailed)));
            }
            ofs << ifs.rdbuf();
            ofs.close();
        }
        ifs.close();
    }

    std::ofstream ofs(tmp, std::ios::app);
    if (!ofs) {
        return result<void>::make(error(
            static_cast<uint32_t>(FailureReason::PassivationSnapshotFailed)));
    }
    ofs << std::hex << record.message_id.value() << "\n";
    ofs.close();
    std::filesystem::rename(tmp, path);
    return result<void>::make();
}

result<void> FileDeliveryStore::mark_outbox_complete(MessageId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::ifstream ifs(outbox_path());
    std::string tmp = outbox_path() + ".tmp";
    std::ofstream ofs(tmp);
    if (!ifs || !ofs) {
        return result<void>::make(error(
            static_cast<uint32_t>(FailureReason::PassivationSnapshotFailed)));
    }
    std::string line;
    while (std::getline(ifs, line)) {
        if (line.empty())
            continue;
        uint64_t mid = std::stoull(line, nullptr, 16);
        if (mid != id.value()) {
            ofs << line << "\n";
        }
    }
    ifs.close();
    ofs.close();
    std::filesystem::rename(tmp, outbox_path());
    return result<void>::make();
}

result<std::vector<PendingSend>> FileDeliveryStore::load_pending_outbox() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<PendingSend> items;
    std::ifstream ifs(outbox_path());
    if (!ifs) {
        return result<std::vector<PendingSend>>::make(std::move(items));
    }
    std::string line;
    while (std::getline(ifs, line)) {
        if (line.empty())
            continue;
        PendingSend send;
        send.message_id = MessageId{std::stoull(line, nullptr, 16)};
        items.push_back(send);
    }
    return result<std::vector<PendingSend>>::make(std::move(items));
}

result<void> FileDeliveryStore::put_inbox(MessageId id, uint64_t ttl_ns) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::ofstream ofs(inbox_path(), std::ios::app);
    if (!ofs) {
        return result<void>::make(error(
            static_cast<uint32_t>(FailureReason::PassivationSnapshotFailed)));
    }
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    ofs << std::hex << id.value() << " "
        << (static_cast<uint64_t>(now) + ttl_ns) << "\n";
    ofs.close();
    return result<void>::make();
}

result<bool> FileDeliveryStore::seen_inbox(MessageId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::ifstream ifs(inbox_path());
    if (!ifs)
        return result<bool>::make(false);
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    std::string line;
    while (std::getline(ifs, line)) {
        if (line.empty())
            continue;
        std::istringstream iss(line);
        uint64_t mid, expiry;
        iss >> std::hex >> mid >> expiry;
        if (mid == id.value()) {
            return result<bool>::make(static_cast<uint64_t>(now) <= expiry);
        }
    }
    return result<bool>::make(false);
}

} // namespace hpactor::mailbox
