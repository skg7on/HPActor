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
#include <hpactor/actor/lifecycle_actor.hpp>
#include <hpactor/actor_context.hpp>
#include <hpactor/messages.pb.h>

#include <iostream>

namespace hpactor {

AbstractActor::AbstractActor(ActorId id, ActorType type, ActorSystem& sys)
    : id_(id), type_(type), system_(sys) {}

void AbstractActor::link_to(const ActorAddr& other) {
    auto* ctx = actor_context();
    if (ctx == nullptr) {
        return;
    }

    // Reject link-to-self
    if (other == address()) {
        std::cerr << "HPActor: link_to self (" << id().value() << ") ignored"
                  << '\n';
        return;
    }

    // Idempotency: check if already linked
    for (const auto& linked : ctx->linked_actors()) {
        if (linked == other) {
            return;
        }
    }

    ctx->add_linked(other);

    // Notify target via LinkMessage
    hpactor::LinkMessage pb;
    pb.set_actor_id(id().value());

    StreamBuffer payload(pb.ByteSizeLong());
    (void)pb.SerializeToArray(payload.data(), static_cast<int>(payload.size()));

    ctx->send(other, TypedMessage(TypeTag::LinkMsg, std::move(payload)));
}

void AbstractActor::unlink_from(const ActorAddr& other) {
    auto* ctx = actor_context();
    if (ctx == nullptr) {
        return;
    }

    ctx->remove_linked(other);

    // Notify target via UnlinkMessage
    hpactor::UnlinkMessage pb;
    pb.set_actor_id(id().value());

    StreamBuffer payload(pb.ByteSizeLong());
    (void)pb.SerializeToArray(payload.data(), static_cast<int>(payload.size()));

    ctx->send(other, TypedMessage(TypeTag::UnlinkMsg, std::move(payload)));
}

void AbstractActor::monitor(const ActorAddr& target) {
    auto* ctx = actor_context();
    if (ctx == nullptr) {
        return;
    }

    // Send MonitorMsg to target. The target's receive() will add us
    // to its monitored_ list so that on_exit() notifies us on death.
    ctx->send(target, TypedMessage(TypeTag::MonitorMsg, StreamBuffer{}));
}

void AbstractActor::demonitor(const ActorAddr& target) {
    auto* ctx = actor_context();
    if (ctx == nullptr) {
        return;
    }

    // Send DemonitorMsg to target. The target's receive() will remove us
    // from its monitored_ list.
    ctx->send(target, TypedMessage(TypeTag::DemonitorMsg, StreamBuffer{}));
}

cli::ActorMeta AbstractActor::to_metadata() const {
    cli::ActorMeta m;
    m.actor_id = id().value();
    m.actor_type = std::string(type_name());
    if (auto* lc = as_lifecycle()) {
        m.state = lc->state_string();
        m.incarnation = lc->incarnation();
    } else {
        m.state = "unknown";
        m.incarnation = address().incarnation;
    }
    return m;
}

void AbstractActor::set_scheduler(sched::IScheduler* /*scheduler*/) {
    // Default no-op; EventBasedActor overrides this
}

void AbstractActor::set_mailbox(mailbox::MPSCActorMailbox<TypedMessage>* /*mailbox*/) {
    // Default no-op; EventBasedActor overrides this
}

} // namespace hpactor
