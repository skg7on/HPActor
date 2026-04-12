#pragma once

#include <hpactor/actor_system.hpp>
#include <hpactor/actor_system_ids.hpp>
#include <hpactor/ref/actor_address.hpp>
#include <hpactor/ref/actor_ref.hpp>
#include <hpactor/spawn.hpp>
#include <hpactor/types.hpp>

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
    template<typename T>
    void register_type(const std::string& name) {
        ActorType id = next_type_id_++;
        types_by_name_[name] = TypeEntry{
            id,
            [](ActorSystem& sys) -> ActorAddress {
                Actor actor = sys.spawn<T>();
                return actor.address();
            }
        };
        names_by_type_[id] = name;
    }

    // Spawn a remote actor by name - returns ActorAddress
    result<ActorAddress> spawn(ActorSystem& system, const std::string& name);

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
    ActorType next_type_id_ = ActorType{100};  // Start after reserved types
};

} // namespace hpactor