// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// ...

#pragma once

#include <hpactor/runtime/actor_spawner.hpp>

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

/// \brief Handles to well-known system actors for wiring by the runtime
///        builder.
///
/// Populated during construction and used to connect system actors
/// (CLI, metrics, receptionist, spawn receiver, HTTP gateway) to other
/// runtime components.
struct SystemActorHandles final {
    Actor spawn_receiver;               ///< Receives remote spawn
                                        ///< requests.
    Actor http_gateway;                 ///< HTTP ingress gateway
                                        ///< actor.
    std::shared_ptr<cli::CliActor> cli; ///< CLI actor (shared
                                        ///< ownership).
    std::shared_ptr<receptionist::Receptionist> receptionist; ///< Actor
                                                              ///< discovery
                                                              ///< receptionist.
    metrics::MetricsActor* metrics{nullptr}; ///< Metrics scrape actor
                                             ///< (non-owning).
};

/// \brief Sole owner of local actor identity, lookup, and adoption.
///
/// Owns the \c ActorDirectory, \c ActorTypeRegistry, \c PassivationManager,
/// and the unified \c ActorSpawner. All actor lifecycle operations
/// (spawn, adopt, lookup, naming, passivation) flow through this component.
///
/// \note Thread safety: The underlying \c ActorDirectory uses internal
///       synchronization. Lookup methods are safe to call from any thread.
///       Mutation methods (adopt, register_name, etc.) must be externally
///       serialized or confined to the startup/bootstrap phase.
class ActorRuntime final {
  public:
    /// \brief Non-owning dependencies that must outlive this component.
    struct Dependencies {
        ActorSystem& facade;          ///< The owning actor system facade.
        EndPoint endpoint;            ///< Local endpoint for this runtime.
        sched::IScheduler& scheduler; ///< Work-stealing scheduler reference.
        metrics::MpscRingBuffer<metrics::MetricEvent>* metrics; ///< Metrics
                                                                ///< ring buffer
                                                                ///< (nullable).
        log::Logger* logger; ///< Structured logger (nullable).
    };

    /// \brief Construct the actor runtime with all owned subsystems.
    ///
    /// \param[in] deps           Stable non-owning references.
    /// \param[in] directory      Owned actor directory (moved in).
    /// \param[in] type_registry  Owned type registry (moved in).
    /// \param[in] passivation    Owned passivation manager (moved in).
    ActorRuntime(Dependencies deps, std::unique_ptr<ActorDirectory> directory,
                 std::unique_ptr<ActorTypeRegistry> type_registry,
                 std::unique_ptr<PassivationManager> passivation) noexcept;

    /// \brief Adopt a constructed local actor through the unified pipeline.
    ///
    /// Delegates to the internal \c ActorSpawner. Publishes a complete
    /// directory entry and binds the actor context.
    ///
    /// \param[in] actor The fully constructed actor (shared ownership).
    /// \param[in] spec  Resolved spawn specification.
    /// \return A valid \c Actor on success, or a typed error code.
    result<Actor>
    adopt(std::shared_ptr<AbstractActor> actor, const SpawnSpec& spec) noexcept;

    // ── Directory lookups ──────────────────────────────────────────────

    /// \brief Find an actor by id.
    /// \return The actor if found, or \c nullptr.
    std::shared_ptr<AbstractActor> find_actor(ActorId id) const noexcept;

    /// \brief Find an actor's mailbox by id.
    /// \return The mailbox if found, or \c nullptr.
    std::shared_ptr<mailbox::MPSCActorMailbox<TypedMessage>>
    find_mailbox(ActorId id) const noexcept;

    /// \brief Find an actor's context by id.
    /// \return The context if found, or \c nullptr.
    std::shared_ptr<ActorContext> find_context(ActorId id) const noexcept;

    /// \brief Immutable snapshot of all directory entries.
    std::vector<ActorDirectoryEntry> snapshot() const;

    /// \brief Current number of registered actors.
    std::size_t actor_count() const noexcept;

    /// \brief Allocate a fresh unique \c ActorId.
    ActorId allocate_id();

    // ── Names ───────────────────────────────────────────────────────────

    /// \brief Register a named alias for an actor address.
    /// \return \c true if the name was registered, \c false if it already
    ///         exists.
    bool register_name(std::string name, ActorAddress address);

    /// \brief Remove a named alias.
    /// \return \c true if the name existed and was removed.
    bool unregister_name(const std::string& name);

    /// \brief Resolve a name to an actor.
    /// \return The actor if the name is registered, or \c std::nullopt.
    std::optional<Actor> resolve_name(const std::string& name) const;

    // ── Directory access (for internal users) ────────────────────────────

    /// \brief Mutable directory access (internal use only).
    ActorDirectory& directory() noexcept {
        return *directory_;
    }
    /// \brief Read-only directory access.
    const ActorDirectory& directory() const noexcept {
        return *directory_;
    }

    // ── Type registry ───────────────────────────────────────────────────

    /// \brief Mutable type registry access (internal use only).
    ActorTypeRegistry& type_registry() noexcept {
        return *type_registry_;
    }

    /// \brief Register an actor type for remote spawn.
    /// \param[in] def The type definition to register.
    void register_actor_type(const ActorTypeDef& def);

    /// \brief Look up a registered actor type.
    /// \param[in] type The type identifier.
    /// \return The type definition, or a default-constructed one if not
    ///         found.
    ActorTypeDef get_actor_type(ActorType type) const;

    // ── Registry view ───────────────────────────────────────────────────

    /// \brief Compatibility view of the actor registry (backed by the
    ///        directory).
    ActorSystem::ActorRegistry& actor_registry();

    // ── Passivation ─────────────────────────────────────────────────────

    /// \brief Passivation manager accessor.
    /// \return The passivation manager, or \c nullptr if disabled.
    PassivationManager* passivation_manager() noexcept {
        return passivation_.get();
    }

    // ── System actor ────────────────────────────────────────────────────

    /// \brief The system actor handle (SpawnReceiver, etc.).
    Actor system_actor() const noexcept {
        return system_actor_;
    }

    /// \brief Set the system actor handle.
    void set_system_actor(Actor a) {
        system_actor_ = a;
    }

    // ── Handles ──────────────────────────────────────────────────────────

    /// \brief Mutable access to all well-known system actor handles.
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
