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

#include <hpactor/net/admin/admin_messages.hpp>

#include <functional>
#include <mutex>
#include <unordered_map>

namespace hpactor {

class ActorSystem;

namespace net::admin {

/// \brief Admin API endpoint handler (OPS-002).
///
/// Provides introspection and control endpoints backed by the live
/// ActorSystem:
/// - GET /admin/actors — enumerate all actors and their lifecycle states
/// - GET /admin/cluster/nodes — list cluster nodes and their states
/// - GET /admin/health — liveness + readiness status
/// - POST /admin/shutdown — initiate graceful shutdown
///
/// Pass nullptr for the system pointer in tests; built-in handlers
/// return error responses when no system is available.
///
/// \note Thread-safe. Designed for wire-up to HTTPGatewayActor or a
///       standalone HTTP server.
class AdminApiActor {
  public:
    /// \brief Callback type for custom admin endpoint handlers.
    using Handler = std::function<AdminResponse(const AdminRequest&)>;

    /// \brief Construct with an optional pointer to the actor system.
    ///
    /// Pass \c nullptr for testing without an ActorSystem; built-in
    /// handlers return error responses in that case.
    ///
    /// \param[in] system Pointer to the live ActorSystem, or \c nullptr.
    explicit AdminApiActor(ActorSystem* system = nullptr);

    /// \brief Register a custom handler for a specific resource.
    ///
    /// \param[in] resource The admin endpoint to handle.
    /// \param[in] handler The callback invoked for requests to \p resource.
    void register_handler(AdminResource resource, Handler handler);

    /// \brief Handle an admin request by dispatching to the registered
    ///        handler or falling back to the built-in implementation.
    ///
    /// \param[in] request The admin request to process.
    /// \return The admin response from the handler or built-in endpoint.
    AdminResponse handle(const AdminRequest& request) const;

    /// \brief Liveness + readiness check.
    ///
    /// \return 200 with \c {"status":"ok"} when running, 503 when not ready,
    ///         500 when no ActorSystem is available.
    AdminResponse health_check() const;

    /// \brief Enumerate all actors in the system.
    ///
    /// \return JSON array of \c {id, type, address} per actor plus
    ///         \c count field.
    AdminResponse list_actors() const;

    /// \brief List cluster nodes and their states.
    ///
    /// \return JSON array of \c {node_id, state} per alive node plus
    ///         \c count field, or a stub when cluster is not enabled.
    AdminResponse list_cluster_nodes() const;

    /// \brief Initiate graceful shutdown.
    ///
    /// \return 200 with \c {"shutdown":"initiated"} on success,
    ///         500 on failure or when no ActorSystem is available.
    AdminResponse do_shutdown() const;

  private:
    struct ResourceHash {
        size_t operator()(AdminResource r) const noexcept {
            return std::hash<int>{}(static_cast<int>(r));
        }
    };
    mutable std::mutex mutex_;
    std::unordered_map<AdminResource, Handler, ResourceHash> handlers_;
    ActorSystem* system_{nullptr};
};

} // namespace net::admin
} // namespace hpactor
