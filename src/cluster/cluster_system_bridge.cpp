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
#include <hpactor/runtime/actor_system_impl.hpp>
#include <hpactor/runtime/network_runtime.hpp>

#include <hpactor/actor/system/actor_system.hpp>
#include <hpactor/cluster/name/name_resolver.hpp>
#include <hpactor/config/name_resolution_config.hpp>

namespace hpactor {

void ActorSystem::enable_cluster(const std::string& node_id) {
    ClusterRuntimeDependencies deps;
    deps.node_id = node_id;

    impl_->cluster_ = create_cluster_runtime(deps, nullptr);
    if (impl_->cluster_) {
        (void)impl_->cluster_->start();
    }

    // ── Distributed name resolution subsystem ─────────────────────────────
    // Wired here so hpactor_lib (actor_system.cpp) never directly calls
    // NameResolver::resolve() — that avoids a circular link dependency
    // (hpactor_lib ↔ hpactor_cluster).  resolve_name_fn is a std::function
    // bridge set by this hpactor_cluster TU and invoked from hpactor_lib.

    config::NameResolutionConfig nr_cfg;
    nr_cfg.enabled = true;

    // Early exit if networking or discovery are unavailable.
    if (!impl_->network_ || !impl_->network_->discovery()) {
        return;
    }

    impl_->name_directory_ = std::make_unique<cluster::name::NameDirectory>();
    impl_->name_resolve_cache_ = std::make_unique<cluster::name::NameResolveCache>();

    // Outbound/inbound ports start as inactive stubs — the integration
    // task (Task 8) will wire real transport-send and frame-dispatch
    // function pointers once the message layer is ready.
    cluster::name::OutboundNameQueryPort outbound_port;
    cluster::name::InboundNamePort inbound_port;

    impl_->name_resolver = std::make_unique<cluster::name::NameResolver>(
        *impl_->name_directory_, *impl_->network_->discovery(),
        *impl_->name_resolve_cache_, nr_cfg, impl_->core.endpoint,
        outbound_port, inbound_port);

    // Bind the bridge so ActorSystem::resolve_actor() can query the
    // name resolver without linking hpactor_lib against NameResolver.
    impl_->resolve_name_fn =
        [nr = impl_->name_resolver.get()](std::string_view name) {
            return nr->resolve(name);
        };
}

} // namespace hpactor
