// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// ...

#include <hpactor/runtime/actor_runtime.hpp>

#include <hpactor/actor/abstract_actor.hpp>
#include <hpactor/actor/actor_context.hpp>
#include <hpactor/mailbox/mpsc_actor_mailbox.hpp>

namespace hpactor {

ActorRuntime::ActorRuntime(Dependencies deps,
                           std::unique_ptr<ActorDirectory> directory,
                           std::unique_ptr<ActorTypeRegistry> type_registry,
                           std::unique_ptr<PassivationManager> passivation) noexcept
    : spawner_(ActorSpawner::Dependencies{
          .facade = deps.facade,
          .endpoint = deps.endpoint,
          .directory = *directory,
          .scheduler = deps.scheduler,
          .metrics = deps.metrics,
          .logger = deps.logger,
      }),
      directory_(std::move(directory)), type_registry_(std::move(type_registry)),
      passivation_(std::move(passivation)),
      registry_(std::make_unique<ActorSystem::ActorRegistry>(*directory_)) {}

result<Actor> ActorRuntime::adopt(std::shared_ptr<AbstractActor> actor,
                                  const SpawnSpec& spec) noexcept {
    return spawner_.adopt(std::move(actor), spec);
}

std::shared_ptr<AbstractActor> ActorRuntime::find_actor(ActorId id) const noexcept {
    auto entry = directory_->find(id);
    return entry.has_value() ? entry->instance : nullptr;
}

std::shared_ptr<mailbox::MPSCActorMailbox<TypedMessage>>
ActorRuntime::find_mailbox(ActorId id) const noexcept {
    return directory_->find_mailbox(id);
}

std::shared_ptr<ActorContext> ActorRuntime::find_context(ActorId id) const noexcept {
    return directory_->find_context(id);
}

std::vector<ActorDirectoryEntry> ActorRuntime::snapshot() const {
    return directory_->snapshot();
}

std::size_t ActorRuntime::actor_count() const noexcept {
    return directory_->size();
}

ActorId ActorRuntime::allocate_id() {
    return directory_->allocate_id();
}

bool ActorRuntime::register_name(std::string name, ActorAddress address) {
    return directory_->register_name(std::move(name), address);
}

bool ActorRuntime::unregister_name(const std::string& name) {
    return directory_->unregister_name(name);
}

std::optional<Actor> ActorRuntime::resolve_name(const std::string& name) const {
    return directory_->resolve_actor(name);
}

void ActorRuntime::register_actor_type(const ActorTypeDef& def) {
    actor_types_[def.id] = def;
}

ActorTypeDef ActorRuntime::get_actor_type(ActorType type) const {
    auto it = actor_types_.find(type);
    return it != actor_types_.end() ? it->second : ActorTypeDef{};
}

ActorSystem::ActorRegistry& ActorRuntime::actor_registry() {
    return *registry_;
}

} // namespace hpactor
