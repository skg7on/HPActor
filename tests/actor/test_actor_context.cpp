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

#include <hpactor/actor_context.hpp>
#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/ref/actor_ref.hpp>

#include <cassert>
#include <type_traits>

using namespace hpactor;

void test_actor_context_children() {
    static_assert(sizeof(hpactor::ActorContext) > 0, "should not be empty");

    // ActorContext should be constructible from an Actor
    Actor empty_actor;
    ActorContext ctx(empty_actor);

    // Children management
    assert(ctx.children().empty());

    Actor child_actor;
    ctx.add_child(child_actor);
    assert(ctx.children().size() == 1);

    ctx.remove_child(child_actor);
    assert(ctx.children().empty());
}

void test_actor_context_linked_actors() {
    Actor empty_actor;
    ActorContext ctx(empty_actor);

    // Linked actors should be empty initially
    assert(ctx.linked_actors().empty());
}

void test_actor_context_monitor() {
    Actor empty_actor;
    ActorContext ctx(empty_actor);

    ActorAddress addr{endpoint_ops::parse_endpoint("node1:12345"), ActorType{2},
                      ActorId{3}, 4};
    ctx.monitor(addr);

    auto monitored = ctx.linked_actors(); // Note: linked_actors, not monitored
    (void)monitored;
}

void test_actor_context_remote_children() {
    Actor empty_actor;
    ActorContext ctx(empty_actor);

    // Remote children should be empty initially
    assert(ctx.remote_children().empty());

    // Create a mock remote actor address
    ActorAddress remote_addr{endpoint_ops::parse_endpoint("node2:12345"),
                             ActorType{10}, ActorId{100}, 1};
    ActorProxy proxy(remote_addr, static_cast<net::Transport*>(nullptr));
    ActorRef remote_child(std::move(proxy));

    // Add remote child
    ctx.add_remote_child(remote_child);
    assert(ctx.remote_children().size() == 1);
}

// -----------------------------------------------------------------------
// Task 5: Behavioral tests for send(), resolve(), and reply()
// -----------------------------------------------------------------------

void test_actor_context_send_with_actor_ref() {
    Config config;
    config.endpoint = endpoint_ops::parse_endpoint("127.0.0.1:0");
    ActorSystem system(config);

    auto actor = system.spawn<EventBasedActor>();
    ActorContext ctx(actor, &system);

    ActorRef target_ref(actor);
    TypedMessage msg(TypeTag::User, bytes{1, 2, 3});
    ctx.send(target_ref, std::move(msg));

    auto* mailbox = system.get_mailbox(actor.address().id);
    assert(mailbox != nullptr);
}

void test_actor_context_send_sets_sender_address() {
    Config config;
    config.endpoint = endpoint_ops::parse_endpoint("127.0.0.1:0");
    ActorSystem system(config);

    auto sender = system.spawn<EventBasedActor>();
    auto target = system.spawn<EventBasedActor>();

    ActorContext ctx(sender, &system);

    TypedMessage msg(TypeTag::User, bytes{42});
    ActorRef target_ref(target);
    ctx.send(target_ref, std::move(msg));

    auto* mailbox = system.get_mailbox(target.address().id);
    assert(mailbox != nullptr);
    TypedMessage received;
    bool popped = mailbox->try_pop(received);
    assert(popped);
    assert(received.sender_address().id == sender.address().id);
}

void test_actor_context_resolve_local() {
    Config config;
    config.endpoint = endpoint_ops::parse_endpoint("127.0.0.1:0");
    ActorSystem system(config);

    auto actor = system.spawn<EventBasedActor>();
    ActorContext ctx(actor, &system);

    ActorRef ref = ctx.resolve(actor.address());
    assert(ref);
    assert(ref.is_local());
    assert(ref.address().id == actor.address().id);
}

void test_actor_context_resolve_remote() {
    Config config;
    config.endpoint = endpoint_ops::parse_endpoint("127.0.0.1:0");
    ActorSystem system(config);

    auto actor = system.spawn<EventBasedActor>();
    ActorContext ctx(actor, &system);

    auto remote_ep = endpoint_ops::parse_endpoint("10.0.0.1:12345");
    ActorAddress remote_addr{remote_ep, ActorType{1}, ActorId{42}, 0};

    ActorRef ref = ctx.resolve(remote_addr);
    assert(ref);
    assert(!ref.is_local());
    assert(ref.address().id == ActorId{42});
}

void test_actor_context_reply() {
    Config config;
    config.endpoint = endpoint_ops::parse_endpoint("127.0.0.1:0");
    ActorSystem system(config);

    auto actor_a = system.spawn<EventBasedActor>();
    auto actor_b = system.spawn<EventBasedActor>();

    ActorContext ctx(actor_a, &system);
    ctx.set_current_sender(actor_b.address());

    TypedMessage reply_msg(TypeTag::User, bytes{99});
    ctx.reply(std::move(reply_msg));

    auto* mailbox = system.get_mailbox(actor_b.address().id);
    assert(mailbox != nullptr);
    TypedMessage received;
    bool popped = mailbox->try_pop(received);
    assert(popped);
    assert(received.sender_address().id == actor_a.address().id);
}

void test_actor_context_reply_no_sender() {
    Config config;
    config.endpoint = endpoint_ops::parse_endpoint("127.0.0.1:0");
    ActorSystem system(config);

    auto actor = system.spawn<EventBasedActor>();
    ActorContext ctx(actor, &system);
    // No current_sender_ set — reply should be no-op, not crash
    TypedMessage reply_msg(TypeTag::User, bytes{99});
    ctx.reply(std::move(reply_msg));
    // Test passes if we reach here without crashing
}

// -----------------------------------------------------------------------
// Task 2: Tests for linked/monitored accessor methods
// -----------------------------------------------------------------------

void test_actor_context_add_remove_linked() {
    Actor empty_actor;
    ActorContext ctx(empty_actor);

    assert(ctx.linked_actors().empty());

    ActorAddress addr1{endpoint_ops::parse_endpoint("127.0.0.1:0"), ActorType{1},
                       ActorId{10}, 0};
    ctx.add_linked(addr1);
    assert(ctx.linked_actors().size() == 1);
    assert(ctx.linked_actors()[0] == addr1);

    // Duplicate add — allowed (idempotency is caller's responsibility)
    ctx.add_linked(addr1);
    assert(ctx.linked_actors().size() == 2);

    ctx.remove_linked(addr1);
    assert(ctx.linked_actors().size() == 1);

    ctx.remove_linked(addr1);
    assert(ctx.linked_actors().empty());

    // Remove non-existent — no-op, no crash
    ctx.remove_linked(addr1);
    assert(ctx.linked_actors().empty());
}

void test_actor_context_add_remove_monitored() {
    Actor empty_actor;
    ActorContext ctx(empty_actor);

    assert(ctx.monitored_actors().empty());

    ActorAddress addr1{endpoint_ops::parse_endpoint("127.0.0.1:0"), ActorType{2},
                       ActorId{20}, 0};
    ctx.add_monitored(addr1);
    assert(ctx.monitored_actors().size() == 1);
    assert(ctx.monitored_actors()[0] == addr1);

    ctx.remove_monitored(addr1);
    assert(ctx.monitored_actors().empty());

    // Remove non-existent — no-op
    ctx.remove_monitored(addr1);
    assert(ctx.monitored_actors().empty());
}

int main() {
    test_actor_context_children();
    test_actor_context_linked_actors();
    test_actor_context_monitor();
    test_actor_context_remote_children();
    test_actor_context_send_with_actor_ref();
    test_actor_context_send_sets_sender_address();
    test_actor_context_resolve_local();
    test_actor_context_resolve_remote();
    test_actor_context_reply();
    test_actor_context_reply_no_sender();
    test_actor_context_add_remove_linked();
    test_actor_context_add_remove_monitored();
    return 0;
}