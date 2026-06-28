// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// ...

#pragma once

#include "actor_spawner.hpp"

#include <hpactor/actor/actor_directory.hpp>
#include <hpactor/actor/actor_type_registry.hpp>
#include <hpactor/actor/lifecycle/passivation_manager.hpp>
#include <hpactor/types/types.hpp>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace hpactor {

class ActorSystem;
namespace cli {
class CliActor;
}
namespace metrics {
class MetricsActor;
}
namespace net {
class HttpClient;
}
namespace receptionist {
class Receptionist;
}

struct SystemActorHandles final {
    Actor spawn_receiver;
    Actor http_gateway;
    std::shared_ptr<cli::CliActor> cli;
    std::shared_ptr<receptionist::Receptionist> receptionist;
    metrics::MetricsActor* metrics{nullptr};
};

/// Sole owner of local actor identity, lookup, and adoption.
class ActorRuntime final {
  public:
    struct Dependencies {
        ActorSystem& facade;
        EndPoint endpoint;
        sched::IScheduler& scheduler;
        metrics::MpscRingBuffer<metrics::MetricEvent>* metrics;
        log::Logger* logger;
    };

    ActorRuntime(Dependencies deps, std::unique_ptr<ActorDirectory> directory,
                 std::unique_ptr<ActorTypeRegistry> type_registry,
                 std::unique_ptr<PassivationManager> passivation) noexcept;

    result<Actor>
    adopt(std::shared_ptr<AbstractActor> actor, const SpawnSpec& spec) noexcept;

    // ── Directory lookups ──────────────────────────────────────────────
    std::shared_ptr<AbstractActor> find_actor(ActorId id) const noexcept;
    std::shared_ptr<mailbox::MPSCActorMailbox<TypedMessage>>
    find_mailbox(ActorId id) const noexcept;
    std::shared_ptr<ActorContext> find_context(ActorId id) const noexcept;
    std::vector<ActorDirectoryEntry> snapshot() const;
    std::size_t actor_count() const noexcept;
    ActorId allocate_id();

    // ── Names ───────────────────────────────────────────────────────────
    bool register_name(std::string name, ActorAddress address);
    bool unregister_name(const std::string& name);
    std::optional<Actor> resolve_name(const std::string& name) const;

    // ── Directory access (for internal users) ────────────────────────────
    ActorDirectory& directory() noexcept {
        return *directory_;
    }
    const ActorDirectory& directory() const noexcept {
        return *directory_;
    }

    // ── Type registry ───────────────────────────────────────────────────
    ActorTypeRegistry& type_registry() noexcept {
        return *type_registry_;
    }
    void register_actor_type(const ActorTypeDef& def);
    ActorTypeDef get_actor_type(ActorType type) const;

    // ── Registry view ───────────────────────────────────────────────────
    ActorSystem::ActorRegistry& actor_registry();

    // ── Passivation ─────────────────────────────────────────────────────
    PassivationManager* passivation_manager() noexcept {
        return passivation_.get();
    }

    // ── System actor ────────────────────────────────────────────────────
    Actor system_actor() const noexcept {
        return system_actor_;
    }
    void set_system_actor(Actor a) {
        system_actor_ = a;
    }

    // ── Handles ──────────────────────────────────────────────────────────
    SystemActorHandles& handles() noexcept {
        return handles_;
    }

  private:
    ActorSpawner spawner_;
    std::unique_ptr<ActorDirectory> directory_;
    std::unique_ptr<ActorTypeRegistry> type_registry_;
    std::unique_ptr<PassivationManager> passivation_;
    std::unique_ptr<ActorSystem::ActorRegistry> registry_;
    std::unordered_map<ActorType, ActorTypeDef> actor_types_;
    Actor system_actor_;
    SystemActorHandles handles_;
};

} // namespace hpactor
