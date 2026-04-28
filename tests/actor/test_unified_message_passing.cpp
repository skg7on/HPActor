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

#include <hpactor/actor/actor_context.hpp>
#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/core/actor_ref_cache.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/mailbox/mpsc_actor_mailbox.hpp>
#include <hpactor/net/frame.hpp>

#include <cassert>
#include <string>

using namespace hpactor;

void test_deliver_remote_bridge() {
    Config config;
    config.endpoint = endpoint_ops::parse_endpoint("127.0.0.1:0");
    ActorSystem system(config);

    auto target = system.spawn<EventBasedActor>();

    net::WireFrame frame;
    frame.sender = ActorAddress{endpoint_ops::parse_endpoint("10.0.0.1:9999"),
                                ActorType{1}, ActorId{99}, 0};
    frame.receiver = target.address();
    frame.type_tag = static_cast<uint32_t>(TypeTag::User);
    frame.payload = bytes{1, 3, 3, 7};

    system.deliver_remote(frame);

    auto* mailbox = system.get_mailbox(target.address().id);
    assert(mailbox != nullptr);
    TypedMessage received;
    bool popped = mailbox->try_pop(received);
    assert(popped);
    assert(received.type_id() == TypeTag::User);
    assert(received.payload().size() == 4);
    assert(received.sender_address().id == ActorId{99});
}

void test_unified_send_reply_loop() {
    Config config;
    config.endpoint = endpoint_ops::parse_endpoint("127.0.0.1:0");
    ActorSystem system(config);

    auto alice = system.spawn<EventBasedActor>();
    auto bob = system.spawn<EventBasedActor>();

    // Alice sends to Bob
    ActorContext alice_ctx(alice, &system);
    TypedMessage msg(TypeTag::User, bytes{42});
    ActorRef bob_ref(bob);
    alice_ctx.send(bob_ref, std::move(msg));

    // Bob receives
    auto* bob_mailbox = system.get_mailbox(bob.address().id);
    assert(bob_mailbox != nullptr);
    TypedMessage received;
    bool popped = bob_mailbox->try_pop(received);
    assert(popped);
    assert(received.sender_address().id == alice.address().id);

    // Bob replies
    ActorContext bob_ctx(bob, &system);
    bob_ctx.set_current_sender(received.sender_address());
    TypedMessage reply_msg(TypeTag::User, bytes{24});
    bob_ctx.reply(std::move(reply_msg));

    // Alice receives reply
    auto* alice_mailbox = system.get_mailbox(alice.address().id);
    assert(alice_mailbox != nullptr);
    TypedMessage reply_received;
    popped = alice_mailbox->try_pop(reply_received);
    assert(popped);
    assert(reply_received.sender_address().id == bob.address().id);
    assert(reply_received.payload()[0] == 24);
}

void test_reply_with_error() {
    Config config;
    config.endpoint = endpoint_ops::parse_endpoint("127.0.0.1:0");
    ActorSystem system(config);

    auto alice = system.spawn<EventBasedActor>();
    auto bob = system.spawn<EventBasedActor>();

    ActorContext bob_ctx(bob, &system);
    bob_ctx.set_current_sender(alice.address());
    bob_ctx.reply_with_error(error(42, "something went wrong"));

    auto* alice_mailbox = system.get_mailbox(alice.address().id);
    assert(alice_mailbox != nullptr);
    TypedMessage reply_received;
    bool popped = alice_mailbox->try_pop(reply_received);
    assert(popped);
    assert(reply_received.type_id() == TypeTag::ErrorMsg);
    assert(reply_received.payload().size() >= 4);
    assert(reply_received.payload()[0] == 0);
    assert(reply_received.payload()[1] == 0);
    assert(reply_received.payload()[2] == 0);
    assert(reply_received.payload()[3] == 42);
    assert(reply_received.sender_address().id == bob.address().id);
}

void test_send_to_self() {
    Config config;
    config.endpoint = endpoint_ops::parse_endpoint("127.0.0.1:0");
    ActorSystem system(config);

    auto actor = system.spawn<EventBasedActor>();
    ActorContext ctx(actor, &system);

    TypedMessage msg(TypeTag::User, bytes{7});
    ActorRef self_ref(actor);
    ctx.send(self_ref, std::move(msg));

    auto* mailbox = system.get_mailbox(actor.address().id);
    assert(mailbox != nullptr);
    TypedMessage received;
    bool popped = mailbox->try_pop(received);
    assert(popped);
    assert(received.sender_address().id == actor.address().id);
}

int main() {
    test_deliver_remote_bridge();
    test_unified_send_reply_loop();
    test_reply_with_error();
    test_send_to_self();
    return 0;
}
