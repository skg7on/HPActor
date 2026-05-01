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

#include <hpactor/core/actor_system.hpp>
#include <hpactor/core/actor_system_ids.hpp>
#include <hpactor/ref/actor_address.hpp>
#include <hpactor/ref/actor_ref.hpp>
#include <hpactor/spawn.hpp>
#include <hpactor/types/types.hpp>

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

namespace hpactor {

// -----------------------------------------------------------------------------
// ActorTypeRegistry - registry of spawnable actor types on each node
// -----------------------------------------------------------------------------
// Stores factories for actor types that can be remotely spawned.
// Types are registered at startup with register_type<T>().
// Template methods are in header; non-template in actor_type_registry.cpp.
class ActorTypeRegistry {
  public:
    ActorTypeRegistry() = default;

    // Register an actor type for remote spawning
    // Template implementation in header
    template <typename T> void register_type(const std::string& name) {
        ActorType id = next_type_id_++;
        types_by_name_[name] = TypeEntry{id, [](ActorSystem& sys) -> ActorAddress {
                                             Actor actor = sys.spawn<T>();
                                             return actor.address();
                                         }};
        names_by_type_[id] = name;
    }

    // Spawn a remote actor by name - returns ActorAddress
    // args and args_type are for future deserialization support
    result<ActorAddress> spawn(ActorSystem& system, const std::string& name,
                               const StreamBuffer& args, TypeTag args_type);

    bool has(const std::string& name) const;
    ActorType type_id(const std::string& name) const;
    std::string type_name(ActorType type) const;

  private:
    struct TypeEntry {
        ActorType type_id;
        std::function<ActorAddress(ActorSystem&)> factory;
    };

    std::unordered_map<std::string, TypeEntry> types_by_name_;
    std::unordered_map<ActorType, std::string> names_by_type_;
    ActorType next_type_id_ = ActorType{100}; // Start after reserved types
};

} // namespace hpactor