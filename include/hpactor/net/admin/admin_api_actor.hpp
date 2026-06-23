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
#include <unordered_map>

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
    void register_handler(AdminResource resource, Handler handler) {
        std::lock_guard<std::mutex> lock(mutex_);
        handlers_[resource] = std::move(handler);
    }

    /// \brief Handle an admin request by dispatching to the registered handler.
    AdminResponse handle(const AdminRequest& request) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = handlers_.find(request.resource);
        if (it != handlers_.end()) {
            return it->second(request);
        }
        // Fall back to built-in handlers for common resources
        switch (request.resource) {
            case AdminResource::Health:
                return health_check();
            case AdminResource::Actors:
                return list_actors();
            case AdminResource::ClusterNodes:
                return list_cluster_nodes();
            case AdminResource::Shutdown:
                return shutdown();
        }
        return AdminResponse{404, R"({"error":"not_found"})"};
    }

    /// \brief Quick health check (liveness probe).
    static AdminResponse health_check() {
        return AdminResponse{200, R"({"status":"ok"})"};
    }

    /// \brief List actor placeholders (for testing).
    static AdminResponse list_actors() {
        return AdminResponse{200, R"({"actors":[]})"};
    }

    /// \brief List cluster node placeholders (for testing).
    static AdminResponse list_cluster_nodes() {
        return AdminResponse{200, R"({"nodes":[]})"};
    }

    /// \brief Shutdown placeholder (for testing).
    static AdminResponse shutdown() {
        return AdminResponse{200, R"({"shutdown":"initiated"})"};
    }

  private:
    struct ResourceHash {
        size_t operator()(AdminResource r) const noexcept {
            return std::hash<int>{}(static_cast<int>(r));
        }
    };
    mutable std::mutex mutex_;
    std::unordered_map<AdminResource, Handler, ResourceHash> handlers_;
};

} // namespace hpactor::net::admin
