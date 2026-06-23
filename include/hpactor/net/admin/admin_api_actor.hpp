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
#include <string>
#include <vector>

namespace hpactor::net::admin {

/// \brief Minimal Admin API endpoint handler (OPS-002 foundation).
///
/// Provides introspection and control endpoints for the actor system:
/// - GET /admin/actors — list actors and lifecycle states
/// - GET /admin/cluster/nodes — list cluster nodes and states
/// - GET /admin/health — liveness + readiness status
/// - POST /admin/shutdown — initiate graceful shutdown
///
/// The actor system binds callbacks for each resource at construction
/// time. Each callback returns a JSON response string.
///
/// \note Thread-safe. Designed for future wire-up to HTTPGatewayActor
///       or a standalone HTTP server.
class AdminApiActor {
  public:
    using Handler = std::function<AdminResponse(const AdminRequest&)>;

    AdminApiActor() = default;

    /// \brief Register a handler for a specific resource.
    void register_handler(AdminResource resource, Handler handler);

    /// \brief Handle an admin request by dispatching to the registered handler.
    AdminResponse handle(const AdminRequest& request) const;

    /// \brief Quick health check (liveness probe).
    /// Returns {"status":"ok"} when the system is live.
    static AdminResponse health_check();

    /// \brief List actor placeholders (for testing).
    static AdminResponse list_actors();

    /// \brief List cluster node placeholders (for testing).
    static AdminResponse list_cluster_nodes();

    /// \brief Shutdown placeholder (for testing).
    static AdminResponse shutdown();

  private:
    mutable std::mutex mutex_;
    std::unordered_map<AdminResource, Handler, std::hash<uint8_t>> handlers_;
};

} // namespace hpactor::net::admin
