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

#include <hpactor/actor/actor_system.hpp>
#include <hpactor/cluster/cluster_failure_model.hpp>
#include <hpactor/net/admin/admin_messages.hpp>

#include <functional>
#include <mutex>
#include <sstream>
#include <unordered_map>

namespace hpactor::net::admin {

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
    using Handler = std::function<AdminResponse(const AdminRequest&)>;

    /// \brief Construct with an optional pointer to the actor system.
    /// Pass nullptr for testing without an ActorSystem.
    explicit AdminApiActor(ActorSystem* system = nullptr) : system_(system) {}

    /// \brief Register a custom handler for a specific resource.
    void register_handler(AdminResource resource, Handler handler) {
        std::lock_guard<std::mutex> lock(mutex_);
        handlers_[resource] = std::move(handler);
    }

    /// \brief Handle an admin request by dispatching to the registered
    ///        handler or falling back to the built-in implementation.
    AdminResponse handle(const AdminRequest& request) const {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = handlers_.find(request.resource);
            if (it != handlers_.end()) {
                return it->second(request);
            }
        }
        switch (request.resource) {
            case AdminResource::Health:
                return health_check();
            case AdminResource::Actors:
                return list_actors();
            case AdminResource::ClusterNodes:
                return list_cluster_nodes();
            case AdminResource::Shutdown:
                return do_shutdown();
        }
        return AdminResponse{404, R"({"error":"not_found"})"};
    }

    // ── Built-in endpoint implementations ─────────────────────────────────

    /// \brief Liveness + readiness check.
    AdminResponse health_check() const {
        if (system_ == nullptr) {
            return AdminResponse{500, R"({"status":"error","reason":"no_system"})"};
        }
        auto phase = system_->shutdown_phase();
        if (phase == ShutdownPhase::Running) {
            return AdminResponse{200, R"({"status":"ok"})"};
        }
        return AdminResponse{503, R"({"status":"not_ready"})"};
    }

    /// \brief Enumerate all actors in the system.
    AdminResponse list_actors() const {
        if (system_ == nullptr) {
            return AdminResponse{500, R"({"status":"error","reason":"no_system"})"};
        }
        std::ostringstream json;
        json << R"({"actors":[)";
        bool first = true;
        system_->for_each_actor([&](ActorId id, AbstractActor& actor) {
            if (!first)
                json << ',';
            first = false;
            json << R"({"id":)" << id.value() << R"(,"type":")"
                 << actor.type_name() << R"(","address":")"
                 << actor.address().to_string() << R"("})";
        });
        json << R"(],"count":)" << system_->actor_count() << '}';
        return AdminResponse{200, json.str()};
    }

    /// \brief List cluster nodes and their states.
    AdminResponse list_cluster_nodes() const {
        if (system_ == nullptr) {
            return AdminResponse{500, R"({"status":"error","reason":"no_system"})"};
        }
        auto* fm = system_->cluster_failure_model();
        if (fm == nullptr) {
            return AdminResponse{
                200, R"({"nodes":[],"count":0,"cluster":"not_enabled"})"};
        }
        auto alive = fm->alive_nodes();
        std::ostringstream json;
        json << R"({"nodes":[)";
        bool first = true;
        for (const auto& node : alive) {
            if (!first)
                json << ',';
            first = false;
            auto state = fm->get_state(node);
            json << R"({"node_id":")" << node << R"(","state":")"
                 << to_string(state) << R"("})";
        }
        json << R"(],"count":)" << alive.size() << '}';
        return AdminResponse{200, json.str()};
    }

    /// \brief Initiate graceful shutdown.
    AdminResponse do_shutdown() const {
        if (system_ == nullptr) {
            return AdminResponse{500, R"({"status":"error","reason":"no_system"})"};
        }
        auto result = system_->shutdown();
        if (result.ok()) {
            return AdminResponse{200, R"({"shutdown":"initiated"})"};
        }
        return AdminResponse{500, R"({"shutdown":"failed"})"};
    }

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

} // namespace hpactor::net::admin
