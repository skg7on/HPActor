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

#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/mailbox/dead_letter_queue.hpp>
#include <hpactor/mailbox/mailbox_policy.hpp>

#include <cassert>

using namespace hpactor;

int main() {
    Config cfg;
    cfg.endpoint = endpoint_ops::parse_endpoint("127.0.0.1:0");
    cfg.scheduler_threads = 0;
    cfg.mailbox.default_capacity = 1;

    ActorSystem system(cfg);
    auto actor = system.spawn<EventBasedActor>();

    auto ok = system.try_deliver_local(
        actor.id(), TypedMessage(TypeTag::User, StreamBuffer{1}));
    assert(ok.accepted());

    auto full = system.try_deliver_local(
        actor.id(), TypedMessage(TypeTag::User, StreamBuffer{2}));
    assert(!full.accepted());
    assert(full.code == mailbox::EnqueueResultCode::Rejected);

    auto missing = system.try_deliver_local(
        ActorId{99999}, TypedMessage(TypeTag::User, StreamBuffer{3}));
    assert(!missing.accepted());
    assert(missing.code == mailbox::EnqueueResultCode::ActorNotFound);

    // Verify dead-letter was captured for ActorNotFound
    auto dl_snap = system.dead_letter_snapshot();
    assert(dl_snap.depth == 1);

    mailbox::DeadLetterRecord dl;
    assert(system.pop_dead_letter(dl));
    assert(dl.reason == mailbox::DeadLetterReason::ActorNotFound);
    assert(dl.type_tag == TypeTag::User);

    // No more dead letters
    assert(!system.pop_dead_letter(dl));

    TypedMessage popped;
    auto* mailbox = system.get_mailbox(actor.id());
    assert(mailbox != nullptr);
    assert(mailbox->try_pop(popped));
    assert(popped.payload()[0] == 1);

    return 0;
}
