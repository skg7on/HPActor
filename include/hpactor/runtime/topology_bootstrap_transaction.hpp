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

#include <hpactor/runtime/configured_actor_provider.hpp>
#include <hpactor/types/types.hpp>

#include <chrono>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace hpactor::config {
struct TopologyModel;
struct ActorDef;
} // namespace hpactor::config

namespace hpactor {

class ActorDirectory;

/// \brief Result of executing a topology bootstrap transaction.
struct TopologyBootstrapResult final {
    uint64_t fingerprint{0};      ///< Effective fingerprint that was committed.
    uint32_t actor_count{0};      ///< Number of actors in the topology.
    uint32_t rollback_error_bits{0}; ///< Bitmask of rollback phases that failed.
};

/// \brief Executes a prepared topology in a transactional manner.
///
/// The algorithm:
/// 1. Pre-allocate a journal with one entry per actor.
/// 2. For each actor in model order: match provider → spawn_unpublished →
///    await_ready → append lease.
/// 3. On all ready: register_names_atomically() → deliver SystemInit.
/// 4. Commit all leases.
/// 5. On any failure: reverse-order rollback with stable rollback error bits.
///
/// The C++ provider wraps \c ActorFactoryRegistry and \c ActorSpawner.
/// An external provider (e.g. Python) communicates through bounded value-only
/// records and integer factory tokens.
class TopologyBootstrapTransaction final {
  public:
    /// \brief Execute a prepared topology.
    ///
    /// \param[in] model           The validated topology model.
    /// \param[in] specs           Configured-actor plans in model order.
    /// \param[in] effective_fp    Effective fingerprint for the blueprint.
    /// \param[in] cpp_provider    Built-in C++ provider port (may be empty
    ///                            when only external actors exist).
    /// \param[in] external_provider  External provider port (may be empty
    ///                               when no Python/ext actors exist).
    /// \param[in] directory       The actor directory for name registration.
    /// \param[in] actor_start_timeout  Per-actor readiness deadline.
    /// \return A \c TopologyBootstrapResult on success, or an error describing
    ///         the first failure.
    [[nodiscard]] static result<TopologyBootstrapResult>
    execute(const config::TopologyModel& model,
            std::span<const ConfiguredActorPlan> specs,
            uint64_t effective_fp,
            const ConfiguredActorProviderPort& cpp_provider,
            const ConfiguredActorProviderPort& external_provider,
            ActorDirectory& directory,
            std::chrono::milliseconds actor_start_timeout) noexcept;
};

} // namespace hpactor
