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

#include <hpactor/runtime/cluster_runtime.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace hpactor {

namespace cluster {
class ClusterFailureModel;
class RouteInvalidation;
} // namespace cluster
namespace cluster::singleton {
class ILeadershipBackend;
class SingletonManagerActor;
} // namespace cluster::singleton

/// \brief Concrete implementation of IClusterRuntime wrapping the three
/// existing cluster components.
class ClusterRuntimeImpl final : public IClusterRuntime {
  public:
    explicit ClusterRuntimeImpl(const ClusterRuntimeDependencies& deps,
                                void* reserved = nullptr) noexcept;
    ~ClusterRuntimeImpl() override;

    result<void> start() noexcept override;
    result<void> stop(const ClusterStopRequest& req) noexcept override;
    void on_member_changed(const net::Member& member, bool joined) noexcept override;
    ClusterSnapshot snapshot() const noexcept override;
    ClusterLegacyViews legacy_views() noexcept override;

  private:
    std::string node_id_;
    std::atomic<bool> started_{false};
    std::atomic<uint64_t> epoch_{0};

    // ── Leadership election configuration ─────────────────────────────
    std::string leadership_mode_{"local"}; // "local" | "external" | "disabled"
    std::string leadership_backend_{"etcd"}; // "etcd" | "consul" | "raft"
    std::vector<std::string> etcd_endpoints_;
    std::string etcd_key_prefix_{"/hpactor"};
    uint32_t etcd_request_timeout_ms_{1000};
    std::string etcd_tls_ca_file_;
    std::string etcd_tls_cert_file_;
    std::string etcd_tls_key_file_;

    std::unique_ptr<cluster::ClusterFailureModel> failure_model_;
    std::unique_ptr<cluster::RouteInvalidation> route_invalidation_;
    std::unique_ptr<cluster::singleton::ILeadershipBackend> etcd_backend_;
    std::unique_ptr<cluster::singleton::SingletonManagerActor> singleton_manager_;
};

/// \brief Factory function registered by the cluster library.
std::unique_ptr<IClusterRuntime>
create_cluster_runtime(const ClusterRuntimeDependencies& deps,
                       void* reserved) noexcept;

} // namespace hpactor
