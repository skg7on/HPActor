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
#include <hpactor/actor_context.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/mailbox/mailbox_policy.hpp>

#include <cassert>

using namespace hpactor;

void test_try_send_accepted() {
    Config cfg;
    cfg.endpoint = endpoint_ops::parse_endpoint("127.0.0.1:0");
    cfg.scheduler_threads = 0;
    cfg.mailbox.default_capacity = 4;

    ActorSystem system(cfg);
    auto sender = system.spawn<EventBasedActor>();
    auto target = system.spawn<EventBasedActor>();
    ActorContext ctx(sender, &system);

    auto ok = ctx.try_send(target.address(),
                           TypedMessage(TypeTag::User, StreamBuffer{1}));
    assert(ok.accepted());
    assert(ok.code == mailbox::EnqueueResultCode::Accepted);

    // Verify message arrived
    auto* mailbox = system.get_mailbox(target.address().id);
    assert(mailbox != nullptr);
    TypedMessage received;
    bool popped = mailbox->try_pop(received);
    assert(popped);
    assert(received.payload().size() == 1);
}

void test_try_send_full_mailbox() {
    Config cfg;
    cfg.endpoint = endpoint_ops::parse_endpoint("127.0.0.1:0");
    cfg.scheduler_threads = 0;
    cfg.mailbox.default_capacity = 4;

    ActorSystem system(cfg);
    auto sender = system.spawn<EventBasedActor>();
    auto target = system.spawn<EventBasedActor>();
    ActorContext ctx(sender, &system);

    // Fill the mailbox (capacity 4)
    for (int i = 0; i < 4; ++i) {
        auto ok = ctx.try_send(target.address(),
                               TypedMessage(TypeTag::User, StreamBuffer{1}));
        assert(ok.accepted());
    }

    // Next message should be rejected (mailbox full)
    auto full = ctx.try_send(target.address(),
                             TypedMessage(TypeTag::User, StreamBuffer{2}));
    assert(!full.accepted());
}

void test_try_send_actor_not_found() {
    Config cfg;
    cfg.endpoint = endpoint_ops::parse_endpoint("127.0.0.1:0");

    ActorSystem system(cfg);
    auto sender = system.spawn<EventBasedActor>();
    ActorContext ctx(sender, &system);

    auto missing_addr = sender.address();
    missing_addr.id = ActorId{99999};
    auto missing =
        ctx.try_send(missing_addr, TypedMessage(TypeTag::User, StreamBuffer{3}));
    assert(missing.code == mailbox::EnqueueResultCode::ActorNotFound);
}

void test_try_send_with_priority() {
    Config cfg;
    cfg.endpoint = endpoint_ops::parse_endpoint("127.0.0.1:0");
    cfg.scheduler_threads = 0;
    cfg.mailbox.default_capacity = 4;

    ActorSystem system(cfg);
    auto sender = system.spawn<EventBasedActor>();
    auto target = system.spawn<EventBasedActor>();
    ActorContext ctx(sender, &system);

    auto ok = ctx.try_send_with_priority(
        target.address(), TypedMessage(TypeTag::User, StreamBuffer{42}),
        /*priority=*/0, /*deadline_ns=*/INT64_MAX);
    assert(ok.accepted());

    // Verify message arrived
    auto* mailbox = system.get_mailbox(target.address().id);
    assert(mailbox != nullptr);
    TypedMessage received;
    bool popped = mailbox->try_pop(received);
    assert(popped);
    assert(received.payload().size() == 1);
}

void test_try_send_sets_sender_address() {
    Config cfg;
    cfg.endpoint = endpoint_ops::parse_endpoint("127.0.0.1:0");
    cfg.scheduler_threads = 0;
    cfg.mailbox.default_capacity = 4;

    ActorSystem system(cfg);
    auto sender = system.spawn<EventBasedActor>();
    auto target = system.spawn<EventBasedActor>();
    ActorContext ctx(sender, &system);

    auto ok = ctx.try_send(target.address(),
                           TypedMessage(TypeTag::User, StreamBuffer{7}));
    assert(ok.accepted());

    // Verify sender address was stamped
    auto* mailbox = system.get_mailbox(target.address().id);
    assert(mailbox != nullptr);
    TypedMessage received;
    bool popped = mailbox->try_pop(received);
    assert(popped);
    assert(received.sender_address().id == sender.address().id);
}

void test_existing_send_still_works() {
    Config cfg;
    cfg.endpoint = endpoint_ops::parse_endpoint("127.0.0.1:0");
    cfg.scheduler_threads = 0;
    cfg.mailbox.default_capacity = 4;

    ActorSystem system(cfg);
    auto sender = system.spawn<EventBasedActor>();
    auto target = system.spawn<EventBasedActor>();
    ActorContext ctx(sender, &system);

    // Existing fire-and-forget send() should still compile and work
    ctx.send(target.address(), TypedMessage(TypeTag::User, StreamBuffer{55}));

    auto* mailbox = system.get_mailbox(target.address().id);
    assert(mailbox != nullptr);
    TypedMessage received;
    bool popped = mailbox->try_pop(received);
    assert(popped);
}

int main() {
    test_try_send_accepted();
    test_try_send_full_mailbox();
    test_try_send_actor_not_found();
    test_try_send_with_priority();
    test_try_send_sets_sender_address();
    test_existing_send_still_works();
    return 0;
}
