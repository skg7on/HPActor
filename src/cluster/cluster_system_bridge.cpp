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

#include <hpactor/actor/actor_system.hpp>
#include <hpactor/cluster/cluster_failure_model.hpp>
#include <hpactor/cluster/singleton/oldest_node_election.hpp>
#include <hpactor/cluster/singleton/singleton_identity.hpp>
#include <hpactor/cluster/singleton/singleton_manager_actor.hpp>

namespace hpactor {

void ActorSystem::enable_cluster(const std::string& node_id) {
    cluster_enabled_ = true;

    auto* fm = new cluster::ClusterFailureModel();
    cluster_failure_model_ = {
        fm,
        +[](void* p) { delete static_cast<cluster::ClusterFailureModel*>(p); }};

    auto* sm = new cluster::singleton::SingletonManagerActor(
        node_id, std::make_unique<cluster::singleton::OldestNodeElection>());
    singleton_manager_ = {
        sm, +[](void* p) {
            delete static_cast<cluster::singleton::SingletonManagerActor*>(p);
        }};

    // Register shard-coordinator as the first managed singleton.
    auto* sm_ptr = static_cast<cluster::singleton::SingletonManagerActor*>(
        singleton_manager_.get());
    sm_ptr->register_singleton(
        cluster::singleton::SingletonIdentity{"shard-coordinator", 0});

    // Wire observer: node state changes trigger singleton election re-runs.
    auto* fm_ptr =
        static_cast<cluster::ClusterFailureModel*>(cluster_failure_model_.get());
    fm_ptr->register_observer([this](const std::vector<std::string>& alive) {
        auto* mgr = static_cast<cluster::singleton::SingletonManagerActor*>(
            singleton_manager_.get());
        if (mgr) {
            mgr->on_node_state_change(alive);
        }
    });
}

} // namespace hpactor
