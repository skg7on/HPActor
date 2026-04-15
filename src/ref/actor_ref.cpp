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

// Actor reference implementation - see actor_ref.hpp

#include <hpactor/core/actor_system.hpp>
#include <hpactor/ref/actor_ref.hpp>

namespace hpactor {

void ActorRef::send(const ActorAddress& target, MessageVariant msg) {
    if (is_local()) {
        Actor* actor = get_actor();
        if (actor) {
            actor->get()->system().deliver_local(target.id, std::move(msg));
        }
    } else {
        // Remote send via proxy - not implemented until Phase 2
        // ActorProxy* proxy = get_proxy();
    }
}

} // namespace hpactor
