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

#include <hpactor/mem/hibernation_registry.hpp>
#include <hpactor/platform.hpp>

#include <cstdlib>
#include <cstring>
#include <sys/mman.h>

namespace hpactor::mem {

HibernationRegistry& HibernationRegistry::instance() {
    static HibernationRegistry hr;
    return hr;
}

void HibernationRegistry::store(ActorId id, HibernationBuffer buf) {
    std::lock_guard<std::mutex> lock(mutex_);
    buf.actor_id = static_cast<uint32_t>(id.value());
    entries_[id.value()] = buf;
}

HibernationBuffer HibernationRegistry::load(ActorId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(id.value());
    if (it == entries_.end()) {
        return HibernationBuffer{};
    }
    HibernationBuffer buf = it->second;
    entries_.erase(it);
    return buf;
}

void HibernationRegistry::remove(ActorId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(id.value());
    if (it != entries_.end()) {
        if (it->second.ptr) {
            munmap(it->second.ptr, it->second.size);
        }
        entries_.erase(it);
    }
}

bool HibernationRegistry::contains(ActorId id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.find(id.value()) != entries_.end();
}

size_t HibernationRegistry::count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}

size_t HibernationRegistry::total_bytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t total = 0;
    for (const auto& [id, buf] : entries_) {
        total += buf.size;
    }
    return total;
}

} // namespace hpactor::mem
