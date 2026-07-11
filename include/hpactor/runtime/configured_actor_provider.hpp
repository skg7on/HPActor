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

#include <hpactor/runtime/actor_spawn_lease.hpp>
#include <hpactor/types/types.hpp>

#include <chrono>
#include <cstdint>
#include <string>

namespace hpactor::config {
struct ActorDef;
} // namespace hpactor::config

namespace hpactor {

/// \brief Distinguishes built-in C++ from external (e.g. Python) providers.
enum class ConfiguredActorProviderKind : uint8_t {
    BuiltinCpp = 0, ///< Resolved via ActorFactoryRegistry.
    External = 1,   ///< Resolved via an external provider port.
};

/// \brief A plan describing how one configured actor should be spawned.
struct ConfiguredActorPlan final {
    size_t topology_index{0};                    ///< Position in the model.
    ConfiguredActorProviderKind provider{ConfiguredActorProviderKind::BuiltinCpp};
    uint64_t provider_token{0};                  ///< External provider token (0 = C++).
    uint64_t args_fingerprint{0};               ///< Fingerprint of args.
};

/// \brief Port through which an external provider participates in topology
///        bootstrap. All entries are function pointers with a \c void* context;
///        no \c std::function, RTTI, exceptions, or \c PyObject are permitted
///        in this header or its consumers.
struct ConfiguredActorProviderPort final {
    /// Opaque provider state (e.g. PythonTopologyProvider*).
    void* context{nullptr};

    /// Return true if this provider can handle \p plan.
    bool (*matches)(void* context, const ConfiguredActorPlan& plan) noexcept {
        nullptr
    };

    /// Spawn an internally unpublished actor. Must return a valid lease.
    /// The lease's rollback will be called on reverse-order cleanup.
    result<ActorSpawnLease> (*spawn_unpublished)(
        void* context, const config::ActorDef& def,
        const ConfiguredActorPlan& plan) noexcept {nullptr};

    /// Block until the previously spawned actor is ready (constructor,
    /// behavior, on_start() for Python). Called only on the startup thread.
    /// \param timeout Per-actor deadline guard.
    result<void> (*await_ready)(void* context, ActorId actor_id,
                                const ConfiguredActorPlan& plan,
                                std::chrono::milliseconds timeout) noexcept {
        nullptr
    };

    /// Roll back a previously spawned actor. Called in reverse order on
    /// transaction failure. Idempotent.
    result<void> (*rollback_actor)(void* context, ActorId actor_id,
                                   const ConfiguredActorPlan& plan) noexcept {
        nullptr
    };
};

} // namespace hpactor
