// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <hpactor/ref/actor_address.hpp>
#include <hpactor/types/types.hpp>

namespace hpactor::cluster::name {

/// \brief Result of a name registration attempt on the home node.
enum class RegisterResult : uint8_t {
    Ok,
    DuplicateName,
    StaleGeneration,
};

/// \brief A single entry in the home-node name directory.
struct NameEntry {
    ActorId actor_id;       ///< The actor's unique ID.
    EndPoint endpoint;      ///< Where the actor actually runs.
    uint64_t generation{0}; ///< Monotonic counter; bumped on re-registration.
    std::chrono::steady_clock::time_point registered_at{}; ///< Registration timestamp.
};

/// \brief Thread-safe store for name→(ActorId, EndPoint) mappings homed on
///        this node.
///
/// Each node's NameDirectory holds entries for names whose consistent-hash
/// home is this node. Entries may reference actors running on any node.
///
/// \note All public methods are internally synchronized via std::mutex.
class NameDirectory {
  public:
    NameDirectory() = default;

    /// \brief Register a name→entry mapping.
    ///
    /// Rejects duplicates and stale generations (gen <= existing.gen).
    /// \param[in] name Actor name to register.
    /// \param[in] entry NameEntry with actor_id, endpoint, generation.
    /// \return RegisterResult::Ok on success, or a typed rejection.
    RegisterResult register_entry(const std::string& name,
                                  const NameEntry& entry);

    /// \brief Resolve a name to its entry.
    /// \param[in] name Actor name.
    /// \return The NameEntry if registered, or std::nullopt.
    std::optional<NameEntry> resolve(const std::string& name) const;

    /// \brief Remove a name from the directory.
    /// \param[in] name Actor name to remove.
    /// \retval true The name was removed.
    /// \retval false The name was not registered.
    bool unregister(const std::string& name);

    /// \brief Remove all entries pointing to a given endpoint.
    ///
    /// Called on node departure to purge entries for actors hosted on the
    /// departed node.
    /// \param[in] ep Endpoint whose entries should be purged.
    /// \return Number of entries removed.
    size_t purge_by_endpoint(EndPoint ep);

    /// \brief Consistent snapshot of all entries.
    /// \return Vector of (name, NameEntry) pairs.
    std::vector<std::pair<std::string, NameEntry>> snapshot() const;

    /// \brief Number of registered entries.
    size_t size() const noexcept;

  private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, NameEntry> entries_;
};

} // namespace hpactor::cluster::name
