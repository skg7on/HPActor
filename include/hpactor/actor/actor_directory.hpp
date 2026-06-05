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

#include <hpactor/actor/actor_context.hpp>
#include <hpactor/mailbox/mpsc_actor_mailbox.hpp>
#include <hpactor/ref/actor_address.hpp>
#include <hpactor/ref/actor_ref.hpp>
#include <hpactor/types/types.hpp>

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace hpactor {

class AbstractActor;

struct ActorDirectoryEntry {
    Actor actor;
    std::shared_ptr<AbstractActor> instance;
    std::shared_ptr<mailbox::MPSCActorMailbox<TypedMessage>> mailbox;
    std::shared_ptr<ActorContext> context;
};

class ActorDirectory {
  public:
    ActorId allocate_id();
    bool insert(ActorDirectoryEntry entry);
    std::optional<ActorDirectoryEntry> find(ActorId id) const;
    std::optional<Actor> find_actor(ActorId id) const;
    std::shared_ptr<mailbox::MPSCActorMailbox<TypedMessage>>
    find_mailbox(ActorId id) const;
    std::shared_ptr<ActorContext> find_context(ActorId id) const;
    bool register_name(std::string name, ActorAddress address);
    std::optional<ActorAddress> resolve_name(const std::string& name) const;
    std::optional<Actor> resolve_actor(const std::string& name) const;
    std::vector<ActorDirectoryEntry> snapshot() const;
    bool erase(ActorId id);
    std::size_t size() const noexcept;

  private:
    mutable std::mutex mutex_;
    uint64_t next_actor_id_{1};
    std::unordered_map<ActorId, ActorDirectoryEntry> entries_;
    std::unordered_map<std::string, ActorAddress> names_;
};

} // namespace hpactor
