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

#include <hpactor/actor/actor_context.hpp>
#include <hpactor/mailbox/disruptor_mailbox_interface.hpp>
#include <hpactor/mailbox/mailbox_kind.hpp>
#include <hpactor/mailbox/mpsc_actor_mailbox.hpp>
#include <hpactor/ref/actor_address.hpp>
#include <hpactor/ref/actor_ref.hpp>
#include <hpactor/types/types.hpp>

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace hpactor {

class AbstractActor;

/// \brief A single entry in the actor directory holding all actor components.
///
/// Bundles the actor handle, instance, mailbox, and context for a
/// registered actor. \c ActorDirectory stores these entries.
struct ActorDirectoryEntry {
    Actor actor; ///< Actor handle (address + proxy).
    std::shared_ptr<AbstractActor> instance; ///< Shared ownership of the actor
                                             ///< instance.
    /// Mailbox backend kind. \c VariableMpsc by default; set to
    /// \c Disruptor for fixed-mailbox actors.
    mailbox::MailboxKind mailbox_kind{mailbox::MailboxKind::VariableMpsc};
    std::shared_ptr<mailbox::MPSCActorMailbox<TypedMessage>> mailbox; ///< Actor's
                                                                      ///< mailbox
                                                                      ///< for
                                                                      ///< message
                                                                      ///< enqueue.
    /// Fixed-mailbox binding (empty for variable actors).
    mailbox::DisruptorMailboxHandle fixed_mailbox;
    std::shared_ptr<ActorContext> context; ///< Actor's execution context.
};

/// \brief Thread-safe registry of live actors, mailboxes, and named addresses.
///
/// Each spawned actor is inserted with its mailbox and context. Lookup by
/// \c ActorId or registered name is O(1) average. Snapshot returns a
/// consistent copy under the internal mutex.
///
/// \note Thread safety: All public methods are internally synchronized via
///       \c std::mutex. Safe to call from any thread.
class ActorDirectory {
  public:
    /// \brief Allocate a fresh, monotonically increasing actor ID.
    ///
    /// \return A new \c ActorId unique within this directory.
    ActorId allocate_id();

    /// \brief Insert an entry into the directory.
    ///
    /// \param[in] entry The fully constructed actor entry to register.
    /// \retval true Entry was inserted.
    /// \retval false An entry for this actor ID already exists.
    bool insert(ActorDirectoryEntry entry);

    /// \brief Status codes for atomic actor-directory publication.
    enum class PublishStatus : uint8_t {
        Published,        ///< Entry and optional name committed atomically.
        DuplicateActorId, ///< An entry with this actor id already exists.
        DuplicateName,    ///< The requested name is already registered.
    };

    /// \brief Atomically publish an entry with an optional registered name.
    ///
    /// Under one mutex, checks for duplicate actor id and name, then commits
    /// both the entry and name mapping or neither.
    ///
    /// \param[in] entry Fully constructed entry to register.
    /// \param[in] name  Optional human-readable name; copied if present.
    /// \return \c Published when both id and name are committed; otherwise
    ///         a typed status describing the conflict.
    PublishStatus publish(ActorDirectoryEntry entry,
                          std::optional<std::string_view> name = std::nullopt);

    /// \brief Find the complete entry for an actor.
    ///
    /// \param[in] id Actor identifier.
    /// \return The \c ActorDirectoryEntry if found, or \c std::nullopt.
    std::optional<ActorDirectoryEntry> find(ActorId id) const;

    /// \brief Find the actor handle by ID.
    ///
    /// \param[in] id Actor identifier.
    /// \return The \c Actor handle if found, or \c std::nullopt.
    std::optional<Actor> find_actor(ActorId id) const;

    /// \brief Find the mailbox for an actor.
    ///
    /// \param[in] id Actor identifier.
    /// \return Shared pointer to the mailbox, or \c nullptr if not found.
    std::shared_ptr<mailbox::MPSCActorMailbox<TypedMessage>>
    find_mailbox(ActorId id) const;

    /// \brief Find the execution context for an actor.
    ///
    /// \param[in] id Actor identifier.
    /// \return Shared pointer to the context, or \c nullptr if not found.
    std::shared_ptr<ActorContext> find_context(ActorId id) const;

    /// \brief Find the fixed-mailbox binding for a fixed-mailbox actor.
    ///
    /// \param[in] id Actor identifier.
    /// \return The \c DisruptorMailboxHandle if the actor uses a fixed
    ///         mailbox, or \c std::nullopt if not found or variable.
    std::optional<mailbox::DisruptorMailboxHandle>
    find_fixed_binding(ActorId id) const;

    /// \brief Register a name-to-address mapping.
    ///
    /// \param[in] name Human-readable actor name.
    /// \param[in] address Actor address to associate with the name.
    /// \retval true Name was registered.
    /// \retval false Name already exists in the directory.
    bool register_name(std::string name, ActorAddress address);

    /// \brief Remove a name-to-address mapping.
    ///
    /// Thread-safe. Returns false when the name is not registered.
    /// \param[in] name The name to unregister.
    /// \retval true The name was erased.
    /// \retval false The name was not registered.
    bool unregister_name(const std::string& name);

    /// \brief Resolve a registered name to an address.
    ///
    /// \param[in] name Previously registered actor name.
    /// \return The \c ActorAddress if found, or \c std::nullopt.
    std::optional<ActorAddress> resolve_name(const std::string& name) const;

    /// \brief Resolve a registered name to an actor handle.
    ///
    /// Combines \c resolve_name() with \c find_actor() to produce
    /// an \c Actor handle for name-based lookup.
    /// \param[in] name Previously registered actor name.
    /// \return The \c Actor handle if found, or \c std::nullopt.
    std::optional<Actor> resolve_actor(const std::string& name) const;

    /// \brief Return a consistent snapshot of all registered entries.
    ///
    /// Iterates the internal map under the lock and copies every entry.
    /// \return A vector of all \c ActorDirectoryEntry values.
    /// \note Linear in the number of registered actors.
    std::vector<ActorDirectoryEntry> snapshot() const;

    /// \brief Remove an entry from the directory.
    ///
    /// \param[in] id Actor identifier to remove.
    /// \retval true An entry was erased.
    /// \retval false No entry existed for this ID.
    bool erase(ActorId id);

    /// \brief Current count of registered entries.
    ///
    /// \return Number of actors in the directory.
    std::size_t size() const noexcept;

  private:
    mutable std::mutex mutex_;
    uint64_t next_actor_id_{1};
    std::unordered_map<ActorId, ActorDirectoryEntry> entries_;
    std::unordered_map<std::string, ActorAddress> names_;
};

} // namespace hpactor
