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

#include <hpactor/actor/abstract_actor.hpp>

namespace hpactor {

AbstractActor::AbstractActor(ActorId id, ActorType type, ActorSystem& sys)
    : id_(id), type_(type), system_(sys) {}

void AbstractActor::link_to(const ActorAddr& /*other*/) {
    // TODO: implement link mechanism
}

void AbstractActor::unlink_from(const ActorAddr& /*other*/) {
    // TODO: implement unlink mechanism
}

void AbstractActor::monitor(const ActorAddr& /*target*/) {
    // TODO: implement monitor mechanism
}

void AbstractActor::demonitor(const ActorAddr& /*target*/) {
    // TODO: implement demonitor mechanism
}

void AbstractActor::set_scheduler(sched::IScheduler* /*scheduler*/) {
    // Default no-op; EventBasedActor overrides this
}

void AbstractActor::set_mailbox(
    mailbox::MPSCActorMailbox<TypedMessage>* /*mailbox*/) {
    // Default no-op; EventBasedActor overrides this
}

} // namespace hpactor
