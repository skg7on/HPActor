#pragma once

#include <hpactor/actor_registry.hpp>
#include <hpactor/actor/abstract_actor.hpp>
#include <hpactor/ref/actor_ref.hpp>
#include <hpactor/types.hpp>

#include <unordered_map>

namespace hpactor {

// -----------------------------------------------------------------------------
// Config - configuration for ActorSystem
// -----------------------------------------------------------------------------
struct Config {
    size_t scheduler_threads = 4;
    size_t max_queue_depth = 1024;
};

// -----------------------------------------------------------------------------
// ActorTypeDef - definition of an actor type
// -----------------------------------------------------------------------------
struct ActorTypeDef {
    std::string name;
    ActorType id;
};

// -----------------------------------------------------------------------------
// ActorSystem - the actor environment containing schedulers, registry, etc.
// -----------------------------------------------------------------------------
class ActorSystem {
public:
    explicit ActorSystem(const Config& config);
    ~ActorSystem();

    // Non-copyable, non-movable
    ActorSystem(const ActorSystem&) = delete;
    ActorSystem& operator=(const ActorSystem&) = delete;
    ActorSystem(ActorSystem&&) = delete;
    ActorSystem& operator=(ActorSystem&&) = delete;

    // Spawn actors at system level
    template<typename Fn, typename... Args>
    Actor spawn(Fn&& fn, Args&&... /*args*/);

    template<typename T, typename... Args>
    T spawn(Args&&... /*args*/);

    // Actor registry
    void register_actor(const std::string& name, Actor actor);
    Actor resolve_actor(const std::string& name);
    void unregister_actor(const std::string& name);

    // Actor type registration
    void register_actor_type(const ActorTypeDef& def);
    ActorTypeDef get_actor_type(ActorType type) const;

    // Clock
    Clock& clock() { return clock_; }

    // System actor
    Actor system_actor() { return system_actor_; }

    // Registry access
    actor_registry& registry() { return registry_; }

private:
    Config config_;
    Clock clock_;
    actor_registry registry_;
    std::unordered_map<ActorType, ActorTypeDef> actor_types_;
    Actor system_actor_;
};

// -----------------------------------------------------------------------------
// Template implementations
// -----------------------------------------------------------------------------

template<typename Fn, typename... Args>
Actor ActorSystem::spawn(Fn&& /*fn*/, Args&&... /*args*/) {
    // TODO: Implement actual actor spawning via scheduler
    static_assert(sizeof(Fn) == 0, "spawn not yet implemented");
    return Actor{};
}

template<typename T, typename... Args>
T ActorSystem::spawn(Args&&... /*args*/) {
    // TODO: Implement actual actor spawning via scheduler
    static_assert(sizeof(T) == 0, "spawn not yet implemented");
    return T{};
}

} // namespace hpactor