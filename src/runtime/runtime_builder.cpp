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

#include <hpactor/runtime/runtime_builder.hpp>

#include <hpactor/actor/actor_system.hpp>

namespace hpactor {

result<RuntimeBuildResult> RuntimeBuilder::build(RuntimeBlueprint bp) noexcept {
    // ── Map blueprint back to Config for the legacy constructor ────────────
    // TODO(Phase 6 Task 5): Replace with proper blueprint-based construction
    // that does NOT start threads/listeners/actors. For now, we use the
    // existing constructor with zero threads and no network to minimize
    // side effects while establishing the RuntimeBuilder API.

    (void)bp; // blueprint fields used in Task 5 integration

    Config config;
    config.scheduler_threads = 0; // no worker threads
    config.enable_network = false;

    auto system = std::unique_ptr<ActorSystem>(new ActorSystem(config));

    RuntimeBuildResult built;
    built.system = std::move(system);
    return result<RuntimeBuildResult>::make(std::move(built));
}

} // namespace hpactor
