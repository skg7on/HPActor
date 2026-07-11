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

#include <hpactor/python/python_bridge_types.hpp>
#include <hpactor/python/python_runtime_snapshot.hpp>
#include <hpactor/runtime/configured_actor_provider.hpp>
#include <hpactor/types/types.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

namespace hpactor::python {

class PythonRuntime;
class PythonNativeSystem;

/// \brief A bounded readiness table for topology actor startup.
///
/// One entry per Python actor in the topology, keyed by factory token.
/// The startup worker calls \c wait_ready(); the Python runtime thread
/// calls \c complete() once the actor is constructed and on_start() finishes.
class PythonTopologyReadyTable final {
  public:
    /// \brief Create a readiness table with at most \p max_entries slots.
    explicit PythonTopologyReadyTable(size_t max_entries) noexcept;

    ~PythonTopologyReadyTable();

    PythonTopologyReadyTable(const PythonTopologyReadyTable&) = delete;
    PythonTopologyReadyTable& operator=(const PythonTopologyReadyTable&) = delete;

    /// \brief Allocate a slot for a factory token.
    ///
    /// \return ok() on success, or an error if the table is full.
    [[nodiscard]] result<void> reserve(uint64_t factory_token) noexcept;

    /// \brief Number of reserved entries.
    [[nodiscard]] size_t size() const noexcept;

    /// \brief Block until the actor identified by \p factory_token is ready.
    ///
    /// Must only be called from the startup worker thread.
    /// \param[in] factory_token The token to wait for.
    /// \param[in] timeout Maximum time to wait.
    /// \return ok() on success, or an error on timeout.
    [[nodiscard]] result<void>
    wait_ready(uint64_t factory_token,
               std::chrono::milliseconds timeout) noexcept;

    /// \brief Signal that an actor has completed startup.
    ///
    /// Must only be called from the Python runtime thread.
    /// \param[in] factory_token The token to complete.
    /// \param[in] system_generation Must match the current system generation.
    /// \param[in] actor_generation Must match the current actor generation.
    /// \param[in] outcome The outcome of the startup.
    /// \param[in] error_code 0 on success, or an error code.
    /// \param[in] detail Bounded detail string (max 4096 bytes).
    /// \return ok() on success.
    [[nodiscard]] result<void>
    complete(uint64_t factory_token, uint64_t system_generation,
             uint64_t actor_generation, TopologyActorOutcome outcome,
             uint32_t error_code, std::string_view detail) noexcept;

  private:
    struct Slot {
        std::mutex mutex;
        std::condition_variable cv;
        bool ready{false};
        TopologyActorOutcome outcome{TopologyActorOutcome::Ready};
        uint32_t error_code{0};
        std::string detail;
    };

    mutable std::mutex table_mutex_;
    std::unordered_map<uint64_t, Slot> entries_;
    size_t max_entries_;
};

/// \brief Value-only provider that bridges the configured-actor topology
///        system to the Python runtime.
///
/// Owns a \c PythonTopologyReadyTable. All methods are called on the
/// startup worker thread except \c complete() which is called from the
/// Python runtime thread. Communicates with Python via bounded dispatch
/// records (integer factory tokens, no CPython objects).
class PythonTopologyProvider final {
  public:
    /// \brief Construct the provider.
    ///
    /// \param[in] runtime The Python runtime for dispatching install/rollback.
    /// \param[in] native_system The owning native system for spawn/rollback.
    /// \param[in] max_actors Maximum number of topology Python actors.
    PythonTopologyProvider(PythonRuntime& runtime,
                           PythonNativeSystem& native_system,
                           size_t max_actors) noexcept;

    ~PythonTopologyProvider();

    PythonTopologyProvider(const PythonTopologyProvider&) = delete;
    PythonTopologyProvider& operator=(const PythonTopologyProvider&) = delete;

    /// \brief Return a \c ConfiguredActorProviderPort bound to this provider.
    [[nodiscard]] ConfiguredActorProviderPort port() noexcept;

    /// \brief The readiness table.
    [[nodiscard]] PythonTopologyReadyTable& ready_table() noexcept;

  private:
    static bool matches(void* ctx, const ConfiguredActorPlan& plan) noexcept;
    static result<ActorSpawnLease>
    spawn_unpublished(void* ctx, const config::ActorDef& def,
                      const ConfiguredActorPlan& plan) noexcept;
    static result<void> await_ready(void* ctx, ActorId id,
                                     const ConfiguredActorPlan& plan,
                                     std::chrono::milliseconds timeout) noexcept;
    static result<void> rollback_actor(void* ctx, ActorId id,
                                        const ConfiguredActorPlan& plan) noexcept;

    PythonRuntime& runtime_;
    PythonNativeSystem& native_system_;
    PythonTopologyReadyTable ready_table_;
};

} // namespace hpactor::python
