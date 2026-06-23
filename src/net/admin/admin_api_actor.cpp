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

#include <hpactor/net/admin/admin_api_actor.hpp>

#include <hpactor/actor/actor_system.hpp>

#include <sstream>

namespace hpactor::net::admin {

AdminApiActor::AdminApiActor(ActorSystem* system) : system_(system) {}

void AdminApiActor::register_handler(AdminResource resource, Handler handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    handlers_[resource] = std::move(handler);
}

AdminResponse AdminApiActor::handle(const AdminRequest& request) const {
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

AdminResponse AdminApiActor::health_check() const {
    if (system_ == nullptr) {
        return AdminResponse{500, R"({"status":"error","reason":"no_system"})"};
    }
    auto phase = system_->shutdown_phase();
    if (phase == ShutdownPhase::Running) {
        return AdminResponse{200, R"({"status":"ok"})"};
    }
    return AdminResponse{503, R"({"status":"not_ready"})"};
}

AdminResponse AdminApiActor::list_actors() const {
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
        json << R"({"id":)" << id.value() << R"(,"type":")" << actor.type_name()
             << R"(","address":")" << actor.address().to_string() << R"("})";
    });
    json << R"(],"count":)" << system_->actor_count() << '}';
    return AdminResponse{200, json.str()};
}

AdminResponse AdminApiActor::list_cluster_nodes() const {
    if (system_ == nullptr) {
        return AdminResponse{500, R"({"status":"error","reason":"no_system"})"};
    }
    // Cluster failure model lives in hpactor_cluster (separate link unit).
    // Cluster node introspection is provided via a registered handler by
    // the cluster subsystem. Without a registered handler, return a stub
    // indicating cluster mode is not enabled.
    return AdminResponse{200, R"({"nodes":[],"count":0,"cluster":"not_enabled"})"};
}

AdminResponse AdminApiActor::do_shutdown() const {
    if (system_ == nullptr) {
        return AdminResponse{500, R"({"status":"error","reason":"no_system"})"};
    }
    auto result = system_->shutdown();
    if (result.ok()) {
        return AdminResponse{200, R"({"shutdown":"initiated"})"};
    }
    return AdminResponse{500, R"({"shutdown":"failed"})"};
}

} // namespace hpactor::net::admin
