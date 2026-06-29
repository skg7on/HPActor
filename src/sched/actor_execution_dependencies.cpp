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

#include <hpactor/sched/actor_execution_dependencies.hpp>

#include <hpactor/actor/actor_system.hpp>

// Access Impl internals to extract directory and DLQ for the dependency bundle.
#include "../runtime/actor_system_impl.hpp"

namespace hpactor::sched {

ActorExecutionDependencies
ActorExecutionDependencies::from(ActorSystem& system) noexcept {
    // Access Impl through the facade's private impl_ member.
    // This is the ONE compatibility adapter permitted to inspect Impl.
    auto& impl = *system.impl_;
    return ActorExecutionDependencies{
        .actors = impl.actors.directory,
        .dead_letters = impl.messaging.dead_letters.get(),
        .use_coroutines = impl.core.config.use_coroutines,
    };
}

} // namespace hpactor::sched
