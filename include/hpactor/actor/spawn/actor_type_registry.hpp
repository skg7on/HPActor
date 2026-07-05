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

#include <hpactor/actor/system/actor_system.hpp>
#include <hpactor/actor/spawn/spawn.hpp>
#include <hpactor/ref/actor_address.hpp>
#include <hpactor/ref/actor_ref.hpp>
#include <hpactor/types/types.hpp>

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

namespace hpactor {

/// \brief Signature for a remote-spawn factory function.
///
/// Receives the target actor system, serialized constructor arguments, and
/// the TypeTag of the argument payload. Returns the spawned \c Actor handle.
using SpawnFactory = std::function<::hpactor::Actor(
    ::hpactor::ActorSystem&, const ::hpactor::StreamBuffer&, ::hpactor::TypeTag)>;

/// \brief Registry of spawnable actor types on each node.
///
/// Stores factories for actor types that can be remotely spawned.
/// Types are registered at startup with \c register_type<T>() or via
/// \c register_factory() for custom construction logic.
///
/// \note Thread safety: Not internally synchronized. Register types before
///       the system begins accepting remote spawn requests.
class ActorTypeRegistry {
  public:
    ActorTypeRegistry() = default;

    /// \brief Register an actor type for remote spawning.
    ///
    /// \tparam T Actor subclass.
    /// \param[in] name Human-readable type name for remote spawn requests.
    template <typename T> void register_type(const std::string& name) {
        ActorType id = next_type_id_++;
        types_by_name_[name] = TypeEntry{
            id, [](ActorSystem& sys, const StreamBuffer&, TypeTag) -> Actor {
                return sys.spawn<T>();
            }};
        names_by_type_[id] = name;
    }

    /// \brief Register a custom factory for remote spawning.
    ///
    /// Allows non-template construction (e.g., with deserialized
    /// constructor arguments) instead of the default \c sys.spawn<T>().
    /// \param[in] name Human-readable type name for remote spawn requests.
    /// \param[in] factory Custom factory matching the \c SpawnFactory
    /// signature.
    void register_factory(const std::string& name, SpawnFactory factory);

    /// \brief Spawn a remote actor by type name.
    ///
    /// \param[in] system Target actor system.
    /// \param[in] name Registered type name.
    /// \param[in] args Serialized constructor arguments.
    /// \param[in] args_type TypeTag of the argument payload.
    /// \return \c result<ActorAddress> with the spawned actor's address,
    ///         or an error if the type name is unknown.
    result<ActorAddress> spawn(ActorSystem& system, const std::string& name,
                               const StreamBuffer& args, TypeTag args_type);

    /// \brief Check whether a type name is registered.
    ///
    /// \param[in] name Type name to look up.
    /// \retval true The name is registered.
    /// \retval false No factory is registered for this name.
    bool has(const std::string& name) const;

    /// \brief Look up the numeric type ID for a registered name.
    ///
    /// \param[in] name Registered type name.
    /// \return The \c ActorType ID, or \c ActorType{0} if not found.
    ActorType type_id(const std::string& name) const;

    /// \brief Look up the type name for a numeric type ID.
    ///
    /// \param[in] type Numeric type tag.
    /// \return The registered name, or an empty string if not found.
    std::string type_name(ActorType type) const;

  private:
    struct TypeEntry {
        ActorType type_id;
        std::function<Actor(ActorSystem&, const StreamBuffer&, TypeTag)> factory;
    };

    std::unordered_map<std::string, TypeEntry> types_by_name_;
    std::unordered_map<ActorType, std::string> names_by_type_;
    ActorType next_type_id_ = ActorType{100}; // Start after reserved types
};

} // namespace hpactor