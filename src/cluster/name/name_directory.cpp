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
#include <hpactor/cluster/name/name_directory.hpp>

#include <mutex>

namespace hpactor::cluster::name {

RegisterResult NameDirectory::register_entry(const std::string& name,
                                             const NameEntry& entry) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(name);
    if (it != entries_.end()) {
        if (entry.generation < it->second.generation) {
            return RegisterResult::StaleGeneration;
        }
        if (entry.generation == it->second.generation) {
            return RegisterResult::DuplicateName;
        }
        // Higher generation — overwrite (re-registration).
        entries_.erase(it);
    }
    entries_.emplace(name, entry);
    return RegisterResult::Ok;
}

std::optional<NameEntry> NameDirectory::resolve(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(name);
    if (it == entries_.end()) {
        return std::nullopt;
    }
    return it->second;
}

bool NameDirectory::unregister(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.erase(name) > 0;
}

size_t NameDirectory::purge_by_endpoint(EndPoint ep) {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t removed = 0;
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (it->second.endpoint == ep) {
            it = entries_.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    return removed;
}

std::vector<std::pair<std::string, NameEntry>>
NameDirectory::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::pair<std::string, NameEntry>> result;
    result.reserve(entries_.size());
    for (const auto& [name, entry] : entries_) {
        result.emplace_back(name, entry);
    }
    return result;
}

size_t NameDirectory::size() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}

} // namespace hpactor::cluster::name
