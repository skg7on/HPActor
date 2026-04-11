#pragma once

#include <hpactor/ref/actor_address.hpp>
#include <hpactor/types.hpp>

#include <unordered_map>

namespace hpactor {

// -----------------------------------------------------------------------------
// actor_registry - maintains a map of actor names to their addresses
// -----------------------------------------------------------------------------
class actor_registry {
public:
    explicit actor_registry(NodeId node_id);

    void put(const std::string& name, ActorAddress addr);
    ActorAddress get(const std::string& name) const;
    void erase(const std::string& name);

private:
    [[maybe_unused]] NodeId node_id_;
    std::unordered_map<std::string, ActorAddress> actors_;
};

} // namespace hpactor