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

#include "cluster_runtime_impl.hpp"

#include <hpactor/cluster/cluster_failure_model.hpp>
#include <hpactor/cluster/route_invalidation.hpp>
#include <hpactor/cluster/singleton/etcd_leadership_backend.hpp>
#include <hpactor/cluster/singleton/leadership_backend_adapter.hpp>
#include <hpactor/cluster/singleton/oldest_node_election.hpp>
#include <hpactor/cluster/singleton/singleton_identity.hpp>
#include <hpactor/cluster/singleton/singleton_manager_actor.hpp>
#include <hpactor/net/service_discovery.hpp> // for net::Member

namespace hpactor {

// ── Factory ────────────────────────────────────────────────────────────────

std::unique_ptr<IClusterRuntime>
create_cluster_runtime(const ClusterRuntimeDependencies& deps,
                       void* /*reserved*/) noexcept {
    return std::make_unique<ClusterRuntimeImpl>(deps);
}

// ── Construction / destruction ─────────────────────────────────────────────

ClusterRuntimeImpl::ClusterRuntimeImpl(const ClusterRuntimeDependencies& deps,
                                       void* /*reserved*/) noexcept
    : node_id_(deps.node_id) {}

ClusterRuntimeImpl::~ClusterRuntimeImpl() = default;

// ── Lifecycle ──────────────────────────────────────────────────────────────

result<void> ClusterRuntimeImpl::start() noexcept {
    bool expected = false;
    if (!started_.compare_exchange_strong(expected, true)) {
        return result<void>::make();
    }
    epoch_.fetch_add(1, std::memory_order_release);

    failure_model_ = std::make_unique<cluster::ClusterFailureModel>();
    route_invalidation_ = std::make_unique<cluster::RouteInvalidation>();

    // Phase 2: config-driven election strategy from [system.cluster.leadership]
    std::unique_ptr<cluster::singleton::ISingletonElection> election;
#ifdef HPACTOR_HAS_GRPC
    if (leadership_mode_ == "external" && leadership_backend_ == "etcd") {
        cluster::singleton::EtcdLeadershipBackend::Config etcd_cfg;
        etcd_cfg.endpoints = etcd_endpoints_;
        etcd_cfg.key_prefix = etcd_key_prefix_;
        etcd_cfg.request_timeout =
            std::chrono::milliseconds(etcd_request_timeout_ms_);
        auto backend = std::make_unique<cluster::singleton::EtcdLeadershipBackend>(
            std::move(etcd_cfg));
        // Note: backend must outlive the adapter. Store in impl.
        etcd_backend_ = std::move(backend);
        auto adapter =
            std::make_unique<cluster::singleton::LeadershipBackendAdapter>(
                node_id_, etcd_backend_.get());
        election = std::move(adapter);
    } else
#endif
    {
        election = std::make_unique<cluster::singleton::OldestNodeElection>();
    }
    singleton_manager_ =
        std::make_unique<cluster::singleton::SingletonManagerActor>(
            node_id_, std::move(election));

    singleton_manager_->register_singleton(
        cluster::singleton::SingletonIdentity{"shard-coordinator", 0});

    // Wire observer: failure-model state changes → singleton election +
    // route invalidation.
    failure_model_->register_observer([this](const std::vector<std::string>& alive) {
        if (singleton_manager_) {
            singleton_manager_->on_node_state_change(alive);
        }
        if (route_invalidation_ && failure_model_) {
            auto invalidated = failure_model_->drain_invalidation_queue();
            if (!invalidated.empty()) {
                route_invalidation_->process(invalidated);
            }
        }
    });

    return result<void>::make();
}

result<void> ClusterRuntimeImpl::stop(const ClusterStopRequest& req) noexcept {
    bool expected = true;
    if (!started_.compare_exchange_strong(expected, false)) {
        return result<void>::make();
    }
    epoch_.fetch_add(1, std::memory_order_release);

    // Destroy in reverse dependency order.
    singleton_manager_.reset();
    etcd_backend_.reset();
    route_invalidation_.reset();
    failure_model_.reset();

    (void)req; // drain_callbacks handled by reset() order
    return result<void>::make();
}

// ── Node events ────────────────────────────────────────────────────────────

void ClusterRuntimeImpl::on_member_changed(const net::Member& /*member*/,
                                           bool /*joined*/) noexcept {
    // Node events are forwarded to the failure model through the existing
    // observer pattern wired in start(). This method serves as the
    // NetworkRuntime → cluster boundary for future integration.
}

// ── Snapshot ───────────────────────────────────────────────────────────────

ClusterSnapshot ClusterRuntimeImpl::snapshot() const noexcept {
    ClusterSnapshot snap;
    snap.enabled = true;
    snap.node_id = node_id_;
    snap.singleton_active = singleton_manager_ != nullptr;
    snap.epoch = epoch_.load(std::memory_order_acquire);
    return snap;
}

// ── Legacy views ───────────────────────────────────────────────────────────

ClusterLegacyViews ClusterRuntimeImpl::legacy_views() noexcept {
    ClusterLegacyViews views;
    views.failure_model = failure_model_.get();
    views.singleton_manager = singleton_manager_.get();
    views.route_invalidation = route_invalidation_.get();
    return views;
}

} // namespace hpactor
