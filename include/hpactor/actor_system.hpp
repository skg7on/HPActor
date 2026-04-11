#pragma once

#include <hpactor/actor/abstract_actor.hpp>
#include <hpactor/actor_registry.hpp>
#include <hpactor/mailbox.hpp>
#include <hpactor/ref/actor_ref.hpp>
#include <hpactor/types.hpp>

#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace hpactor {

// Forward declaration
class Scheduler;

// -----------------------------------------------------------------------------
// Config - configuration for ActorSystem
// -----------------------------------------------------------------------------
struct Config {
    size_t scheduler_threads = 4;
    size_t max_queue_depth = 1024;
    NodeId node_id = LocalNodeId;
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
    template <typename T, typename... Args> Actor spawn(Args&&... args);

    // Actor registry
    void register_actor(const std::string& name, Actor actor);
    Actor resolve_actor(const std::string& name);
    void unregister_actor(const std::string& name);

    // Actor type registration
    void register_actor_type(const ActorTypeDef& def);
    ActorTypeDef get_actor_type(ActorType type) const;

    // Clock
    Clock& clock() {
        return clock_;
    }

    // System actor
    Actor system_actor() {
        return system_actor_;
    }

    // Registry access
    actor_registry& registry() {
        return registry_;
    }

    // Node ID
    NodeId node_id() const { return node_id_; }

    // Internal actor lookup (used by scheduler)
    std::shared_ptr<AbstractActor> get_actor(ActorId id);

    // Get actor's mailbox (used by scheduler)
    ActorMailbox<MessageVariant>* get_mailbox(ActorId id);

    // Deliver message to local actor
    void deliver_local(ActorId target, MessageVariant msg);

  private:
    friend class Scheduler;

    Config config_;
    NodeId node_id_;
    Clock clock_;
    actor_registry registry_;
    std::unordered_map<ActorType, ActorTypeDef> actor_types_;
    Actor system_actor_;

    // Actor registry - maps ActorId to actor instance
    std::unordered_map<ActorId, std::shared_ptr<AbstractActor>> actors_;
    std::mutex actors_mutex_;

    // Actor mailboxes - maps ActorId to mailbox
    std::unordered_map<ActorId, std::unique_ptr<ActorMailbox<MessageVariant>>> mailboxes_;
    std::mutex mailboxes_mutex_;

    // Actor ID generator
    std::atomic<ActorId::counter_type> next_actor_id_{1};

    // Scheduler
    std::unique_ptr<Scheduler> scheduler_;
};

// -----------------------------------------------------------------------------
// Template implementations
// -----------------------------------------------------------------------------

template <typename T, typename... Args>
Actor ActorSystem::spawn(Args&&... args) {
    ActorId id(next_actor_id_.fetch_add(1));
    auto actor = std::make_shared<T>(nullptr, *this, std::forward<Args>(args)...);
    actor->set_address(ActorAddress(node_id_, actor->type(), id, 0));

    {
        std::lock_guard<std::mutex> lock(actors_mutex_);
        actors_.emplace(id, actor);
    }

    // Create mailbox
    {
        std::lock_guard<std::mutex> lock(mailboxes_mutex_);
        mailboxes_.emplace(id, std::make_unique<ActorMailbox<MessageVariant>>());
    }

    return Actor(actor);
}

} // namespace hpactor