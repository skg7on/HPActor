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
    return publish(std::move(entry)) == PublishStatus::Published;
}

ActorDirectory::PublishStatus
ActorDirectory::publish(ActorDirectoryEntry entry,
                        std::optional<std::string_view> name) {
    std::lock_guard<std::mutex> lock(mutex_);
    const ActorId id = entry.actor.id();
    if (entries_.contains(id)) {
        return PublishStatus::DuplicateActorId;
    }
    if (name.has_value() && names_.contains(std::string{*name})) {
        return PublishStatus::DuplicateName;
    }

    const ActorAddress address = entry.actor.address();
    entries_.emplace(id, std::move(entry));
    if (name.has_value()) {
        names_.emplace(std::string{*name}, address);
    }
    return PublishStatus::Published;
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

std::optional<mailbox::DisruptorMailboxHandle>
ActorDirectory::find_fixed_binding(ActorId id) const {
    auto entry = find(id);
    if (!entry.has_value() ||
        entry->mailbox_kind != mailbox::MailboxKind::Disruptor) {
        return std::nullopt;
    }
    return entry->fixed_mailbox;
}

bool ActorDirectory::register_name(std::string name, ActorAddress address) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto [it, inserted] = names_.emplace(std::move(name), address);
    return inserted;
}

bool ActorDirectory::unregister_name(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    return names_.erase(name) > 0;
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
    bool erased = entries_.erase(id) > 0;
    for (auto it = names_.begin(); it != names_.end();) {
        if (it->second.id == id) {
            it = names_.erase(it);
        } else {
            ++it;
        }
    }
    return erased;
}

std::size_t ActorDirectory::size() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}

} // namespace hpactor
