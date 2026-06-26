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

#include <hpactor/actor/abstract_actor.hpp>
#include <hpactor/actor/actor_directory.hpp>

namespace hpactor {

ActorId ActorDirectory::allocate_id() {
    std::lock_guard<std::mutex> lock(mutex_);
    return ActorId{next_actor_id_++};
}

bool ActorDirectory::insert(ActorDirectoryEntry entry) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto id = entry.actor.id();
    auto [it, inserted] = entries_.emplace(id, std::move(entry));
    if (inserted) {
        // Populate the lock-free fast index so hot-path lookups can
        // bypass the mutex.  The raw pointer is stable for the actor's
        // lifetime (entries are never erased).
        ensure_fast_index_capacity(id.value());
        fast_mailboxes_[id.value()].store(it->second.mailbox.get(),
                                          std::memory_order_release);
    }
    return inserted;
}

void ActorDirectory::ensure_fast_index_capacity(uint64_t id_val) {
    if (id_val < fast_mailboxes_capacity_)
        return;
    // Grow with headroom to avoid repeated reallocations.
    size_t new_cap = id_val + 1024;
    auto new_array =
        std::make_unique<std::atomic<mailbox::MPSCActorMailbox<TypedMessage>*>[]>(
            new_cap);
    // Copy existing entries.
    for (size_t i = 0; i < fast_mailboxes_capacity_; ++i) {
        new_array[i].store(fast_mailboxes_[i].load(std::memory_order_relaxed),
                           std::memory_order_relaxed);
    }
    // Zero-initialize new entries.
    for (size_t i = fast_mailboxes_capacity_; i < new_cap; ++i) {
        new_array[i].store(nullptr, std::memory_order_relaxed);
    }
    fast_mailboxes_ = std::move(new_array);
    fast_mailboxes_capacity_ = new_cap;
}

mailbox::MPSCActorMailbox<TypedMessage>*
ActorDirectory::find_mailbox_fast(ActorId id) const noexcept {
    const uint64_t idx = id.value();
    if (idx < fast_mailboxes_capacity_) {
        return fast_mailboxes_[idx].load(std::memory_order_acquire);
    }
    return nullptr;
}

std::optional<ActorDirectoryEntry> ActorDirectory::find(ActorId id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(id);
    if (it == entries_.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::optional<Actor> ActorDirectory::find_actor(ActorId id) const {
    auto entry = find(id);
    if (!entry.has_value()) {
        return std::nullopt;
    }
    return entry->actor;
}

std::shared_ptr<mailbox::MPSCActorMailbox<TypedMessage>>
ActorDirectory::find_mailbox(ActorId id) const {
    auto entry = find(id);
    if (!entry.has_value()) {
        return nullptr;
    }
    return entry->mailbox;
}

std::shared_ptr<ActorContext> ActorDirectory::find_context(ActorId id) const {
    auto entry = find(id);
    if (!entry.has_value()) {
        return nullptr;
    }
    return entry->context;
}

bool ActorDirectory::register_name(std::string name, ActorAddress address) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto [it, inserted] = names_.emplace(std::move(name), address);
    return inserted;
}

std::optional<ActorAddress>
ActorDirectory::resolve_name(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = names_.find(name);
    if (it == names_.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::optional<Actor> ActorDirectory::resolve_actor(const std::string& name) const {
    auto addr = resolve_name(name);
    if (!addr.has_value()) {
        return std::nullopt;
    }
    return find_actor(addr->id);
}

std::vector<ActorDirectoryEntry> ActorDirectory::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ActorDirectoryEntry> result;
    result.reserve(entries_.size());
    for (const auto& [id, entry] : entries_) {
        result.push_back(entry);
    }
    return result;
}

bool ActorDirectory::erase(ActorId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.erase(id) > 0;
}

std::size_t ActorDirectory::size() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}

} // namespace hpactor
