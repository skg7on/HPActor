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
#include <hpactor/actor/scoped_actor.hpp>

namespace hpactor {

ScopedActor::ScopedActor(ActorSystem& sys) : BlockingActor(nullptr, sys) {
    // ScopedActor is used outside the normal spawn path (e.g., from main()).
    // The ActorSystem may not be fully started, so we only set up the
    // blocking actor infrastructure here.  Full registration with mailbox,
    // scheduler, and metrics happens in on_activate() if needed.
    //
    // For MVP: the actor is usable as a blocking receiver.  Messages sent
    // to it from spawned actors will be delivered through its mailbox once
    // the system is running.
}

ScopedActor::~ScopedActor() {
    on_deactivate();
}

} // namespace hpactor
