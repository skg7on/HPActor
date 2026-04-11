#include <hpactor/actor_system.hpp>
#include <hpactor/scheduler.hpp>

namespace hpactor {

// -----------------------------------------------------------------------------
// actor_registry implementation
// -----------------------------------------------------------------------------
actor_registry::actor_registry(NodeId node_id) : node_id_(node_id) {}

void actor_registry::put(const std::string& name, ActorAddress addr) {
    actors_[name] = addr;
}

ActorAddress actor_registry::get(const std::string& name) const {
    auto it = actors_.find(name);
    if (it != actors_.end()) {
        return it->second;
    }
    return invalid_actor_addr;
}

void actor_registry::erase(const std::string& name) {
    actors_.erase(name);
}

// -----------------------------------------------------------------------------
// ActorSystem implementation
// -----------------------------------------------------------------------------
ActorSystem::ActorSystem(const Config& config)
    : config_(config),
      node_id_(config.node_id),
      registry_(node_id_),
      scheduler_(std::make_unique<Scheduler>(*this, config.scheduler_threads)) {
    scheduler_->start();
}

ActorSystem::~ActorSystem() {
    scheduler_->stop();
}

void ActorSystem::register_actor(const std::string& name, Actor actor) {
    registry_.put(name, actor.address());
}

Actor ActorSystem::resolve_actor(const std::string& name) {
    ActorAddress addr = registry_.get(name);
    if (!addr) {
        return Actor{};
    }
    // Return an actor handle - actual resolution would require more
    // infrastructure
    return Actor{};
}

void ActorSystem::unregister_actor(const std::string& name) {
    registry_.erase(name);
}

void ActorSystem::register_actor_type(const ActorTypeDef& def) {
    actor_types_[def.id] = def;
}

ActorTypeDef ActorSystem::get_actor_type(ActorType type) const {
    auto it = actor_types_.find(type);
    if (it != actor_types_.end()) {
        return it->second;
    }
    return ActorTypeDef{};
}

std::shared_ptr<AbstractActor> ActorSystem::get_actor(ActorId id) {
    std::lock_guard<std::mutex> lock(actors_mutex_);
    auto it = actors_.find(id);
    if (it != actors_.end()) {
        return it->second;
    }
    return nullptr;
}

ActorMailbox<MessageVariant>* ActorSystem::get_mailbox(ActorId id) {
    std::lock_guard<std::mutex> lock(mailboxes_mutex_);
    auto it = mailboxes_.find(id);
    if (it != mailboxes_.end()) {
        return it->second.get();
    }
    return nullptr;
}

void ActorSystem::deliver_local(ActorId target, MessageVariant msg) {
    ActorMailbox<MessageVariant>* mailbox = nullptr;
    {
        std::lock_guard<std::mutex> lock(mailboxes_mutex_);
        auto it = mailboxes_.find(target);
        if (it != mailboxes_.end()) {
            mailbox = it->second.get();
        }
    }

    if (mailbox) {
        mailbox->push(Message<MessageVariant>(std::move(msg)));
        scheduler_->enqueue(target, MessageVariant{});  // Enqueue for processing
    }
}

} // namespace hpactor
