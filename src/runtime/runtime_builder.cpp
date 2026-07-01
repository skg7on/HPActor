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
    // ── Blueprint-based construction ────────────────────────────────────────
    // Uses the FromBlueprint constructor which builds all components from
    // the validated blueprint but does NOT start threads, listeners, or
    // actors.  The RuntimeCoordinator owns startup ordering (Phase 6 Task 5).

    auto system = std::unique_ptr<ActorSystem>(
        new ActorSystem(ActorSystem::FromBlueprint{}, bp));

    RuntimeBuildResult built;
    built.system = std::move(system);
    return result<RuntimeBuildResult>::make(std::move(built));
}

} // namespace hpactor
