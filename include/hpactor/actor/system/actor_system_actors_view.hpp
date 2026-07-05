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

#include <cstddef>
#include <optional>
#include <string>

#include <hpactor/ref/actor_ref.hpp>
#include <hpactor/types/types.hpp>

namespace hpactor {

class ActorDirectory;
class ActorTypeRegistry;

/// \brief Non-owning read-only view of the actor subsystem.
///
/// Obtained from \c ActorSystem::actors_view().  Safe for inspection;
/// lifetime must not exceed the parent \c ActorSystem.
///
/// \note All methods are thread-safe (delegate to synchronized internals).
class ActorSystemActorsView final {
  public:
    /// \brief Number of currently registered actors.
    [[nodiscard]] std::size_t actor_count() const noexcept;

    /// \brief Look up an actor by id.  Returns \c std::nullopt if not found.
    [[nodiscard]] std::optional<Actor> find_actor(ActorId id) const;

    /// \brief Resolve an actor by registered name.
    [[nodiscard]] std::optional<Actor> resolve_actor(const std::string& name) const;

    /// \brief The well-known system actor handle.
    [[nodiscard]] Actor system_actor() const noexcept;

    /// \brief The actor type registry (for remote spawn).
    [[nodiscard]] const ActorTypeRegistry& actor_type_registry() const noexcept;

  private:
    friend class ActorSystem;

    ActorSystemActorsView(const ActorDirectory& directory,
                          const ActorTypeRegistry& type_registry,
                          Actor system_actor) noexcept;

    const ActorDirectory* directory_;
    const ActorTypeRegistry* type_registry_;
    Actor system_actor_;
};

} // namespace hpactor
