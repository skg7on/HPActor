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

#include "../runtime/actor_system_impl.hpp"
#include "cluster_runtime_impl.hpp"

#include <hpactor/actor/system/actor_system.hpp>

namespace hpactor {

void ActorSystem::enable_cluster(const std::string& node_id) {
    ClusterRuntimeDependencies deps;
    deps.node_id = node_id;

    impl_->cluster_ = create_cluster_runtime(deps, nullptr);
    if (impl_->cluster_) {
        (void)impl_->cluster_->start();
    }
}

} // namespace hpactor
