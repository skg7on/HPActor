#include <hpactor/actor_type_registry.hpp>
#include <hpactor/actor_system.hpp>

namespace hpactor {

result<ActorAddress> ActorTypeRegistry::spawn(ActorSystem& system,
                                              const std::string& name) {
    auto it = types_by_name_.find(name);
    if (it == types_by_name_.end()) {
        return result<ActorAddress>::make(error(spawn_errors::unknown_type,
                                                  "unknown actor type: " + name));
    }

    ActorAddress addr = it->second.factory(system);
    return result<ActorAddress>::make(std::move(addr));
}

bool ActorTypeRegistry::has(const std::string& name) const {
    return types_by_name_.find(name) != types_by_name_.end();
}

ActorType ActorTypeRegistry::type_id(const std::string& name) const {
    auto it = types_by_name_.find(name);
    if (it != types_by_name_.end()) {
        return it->second.type_id;
    }
    return ActorType{0};
}

std::string ActorTypeRegistry::type_name(ActorType type) const {
    auto it = names_by_type_.find(type);
    if (it != names_by_type_.end()) {
        return it->second;
    }
    return "";
}

} // namespace hpactor