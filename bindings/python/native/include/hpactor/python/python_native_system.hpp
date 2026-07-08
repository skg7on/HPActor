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
#include <hpactor/python/python_ports.hpp>
#include <hpactor/python/python_reliability.hpp>
#include <hpactor/python/python_runtime.hpp>
#include <hpactor/ref/actor_ref.hpp>
#include <hpactor/types/types.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

namespace hpactor {

class ActorSystem;
struct Config;

} // namespace hpactor

namespace hpactor::python {

class PythonCommandRouter;
class PythonGatewayWakeAdapter;

/// \brief Return value for spawn_bridge().
struct PythonSpawnedActor final {
    ActorAddress address;
    uint64_t generation{0};
};

/// \brief Value-only ownership facade for the Python binding subsystem.
///
/// Owns the \c ActorSystem, \c PythonRuntime, \c PythonCommandRouter, gateway
/// actor, wake adapter, and one hidden application bridge. All methods return
/// explicit \c result values; no exceptions, no Python objects, no RTTI.
///
/// Lifecycle: create → start → (use) → begin_draining → stop.
/// stop() is idempotent and safe to call after any state.
class PythonNativeSystem final {
  public:
    /// \brief Create all owned components in destruction-safe order.
    ///
    /// \param[in] system_config  ActorSystem configuration (copied).
    /// \param[in] python_config  Python bridge runtime configuration (copied).
    /// \return A unique_ptr to the system, or an error.
    [[nodiscard]] static result<std::unique_ptr<PythonNativeSystem>>
    create(Config system_config, PythonRuntimeConfig python_config) noexcept;

    ~PythonNativeSystem();

    PythonNativeSystem(const PythonNativeSystem&) = delete;
    PythonNativeSystem& operator=(const PythonNativeSystem&) = delete;

    /// \brief Start the gateway, application bridge, and Python runtime.
    ///
    /// Spawns the gateway actor and one hidden application bridge, then
    /// starts the Python runtime with the wake adapter. Rolls back partial
    /// startup on failure.
    ///
    /// \return ok() on success, or an error.
    [[nodiscard]] result<void> start() noexcept;

    /// \brief Begin draining the Python runtime.
    ///
    /// Transitions the runtime to Draining. No new subcommands accepted.
    /// \return ok() on success.
    [[nodiscard]] result<void> begin_draining() noexcept;

    /// \brief Stop the entire system.
    ///
    /// Idempotent. Stops runtime, bridges, gateway, and actor system in
    /// reverse dependency order.
    ///
    /// \return ok() on success.
    [[nodiscard]] result<void> stop() noexcept;

    /// \brief Spawn a new child PythonBridgeActor.
    ///
    /// Reserves a lease, spawns the bridge under the application bridge,
    /// binds the lease on activation.
    ///
    /// \return The spawned actor's address and generation, or an error.
    [[nodiscard]] result<PythonSpawnedActor> spawn_bridge() noexcept;

    /// \brief Stop a specific bridge actor by address.
    ///
    /// \param[in] actor The address of the bridge to stop.
    /// \return ok() on success, or an error.
    [[nodiscard]] result<void> stop_bridge(ActorAddress actor) noexcept;

    /// \brief Register a human-readable name → ActorAddress mapping.
    ///
    /// \param[in] name  The name (max 255 bytes, non-empty ASCII).
    /// \param[in] actor The actor address to associate.
    /// \return ok() on success, or an error.
    [[nodiscard]] result<void>
    register_name(std::string_view name, ActorAddress actor) noexcept;

    /// \brief Resolve a name to an ActorAddress.
    ///
    /// \param[in] name The name to look up.
    /// \return The registered address, or a default-constructed address if
    ///         not found.
    [[nodiscard]] ActorAddress resolve_name(std::string_view name) const noexcept;

    /// \brief The hidden application bridge's address.
    ///
    /// \return The application origin address.
    [[nodiscard]] ActorAddress application_origin() const noexcept;

    /// \brief The application bridge's generation.
    ///
    /// \return The monotonic generation of the application bridge.
    [[nodiscard]] uint64_t application_generation() const noexcept;

    /// \brief Submit a command to the runtime.
    ///
    /// \param[in] command Shared pointer to the command.
    /// \return true if the command was enqueued.
    [[nodiscard]] bool submit(const PythonCommandPtr& command) noexcept;

    /// \brief Read-end fd for the dispatch notifier.
    [[nodiscard]] int dispatch_read_fd() const noexcept;

    /// \brief Read-end fd for the completion notifier.
    [[nodiscard]] int completion_read_fd() const noexcept;

    /// \brief Drain up to \p max_items dispatch envelopes.
    template <typename Fn> size_t drain_dispatch(size_t max_items, Fn&& fn) {
        if (!runtime_)
            return 0;
        return runtime_->drain_dispatch(max_items, std::forward<Fn>(fn));
    }

    /// \brief Drain up to \p max_items completions.
    template <typename Fn> size_t drain_completions(size_t max_items, Fn&& fn) {
        if (!runtime_)
            return 0;
        return runtime_->drain_completions(max_items, std::forward<Fn>(fn));
    }

    /// \brief Return a point-in-time runtime snapshot.
    [[nodiscard]] PythonRuntimeSnapshot snapshot() const noexcept;

  private:
    PythonNativeSystem(Config system_config,
                       PythonRuntimeConfig python_config) noexcept;

    std::unique_ptr<ActorSystem> system_;
    std::unique_ptr<PythonRuntime> runtime_;
    std::unique_ptr<PythonCommandRouter> router_;
    std::unique_ptr<PythonGatewayWakeAdapter> wake_adapter_;
    PythonReliabilityController reliability_;
    Actor application_bridge_;
    Actor gateway_;
    std::unordered_map<std::string, ActorAddress> name_registry_;
    uint64_t app_generation_{0};
};

} // namespace hpactor::python
