// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#include <hpactor/actor/abstract_actor.hpp>
#include <hpactor/actor/drain_config.hpp>
#include <hpactor/actor/drain_policy.hpp>
#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/actor/lifecycle_actor.hpp>
#include <hpactor/actor/typed_message.hpp>
#include <hpactor/behavior.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/mailbox/dead_letter_queue.hpp>
#include <hpactor/mem/memory_config.hpp>
#include <hpactor/types/types.hpp>

#include <cassert>
#include <iostream>
#include <memory>

using namespace hpactor;

// ── Test actor with lifecycle and counting handlers ─────────────────────────

class DrainTestActor : public EventBasedActor, public LifecycleActor {
  public:
    int user_handler_count = 0;
    int system_handler_count = 0;

    DrainTestActor(ActorContext* ctx, ActorSystem& sys)
        : EventBasedActor(ctx, sys) {
        become(make_behavior());
    }

    LifecycleActor* as_lifecycle() override {
        return this;
    }
    const LifecycleActor* as_lifecycle() const override {
        return this;
    }

    Behavior make_behavior() override {
        return Behavior([this](TypedMessage& msg) {
            if (static_cast<uint32_t>(msg.type_id()) < 0x1000) {
                system_handler_count++;
            } else {
                user_handler_count++;
            }
        });
    }
};

// ── Helper: inject a test message into an actor's mailbox ────────────────────

static void inject_message(EventBasedActor* actor, TypeTag tag) {
    auto* mailbox = actor->get_mailbox();
    assert(mailbox != nullptr);

    // Allocate a TypedMessage node via the custom allocator
    auto* node = static_cast<TypedMessage*>(mem::allocate(
        mem::RegionType::kMessage, sizeof(TypedMessage), actor->id()));
    new (node) TypedMessage(tag, StreamBuffer{});
    node->set_sender_address(ActorAddress{});

    mailbox->inject_for_test(node);
}

// ── Helper: enqueue a non-intercepted system message ────────────────────────
// Uses a TypeTag value < 0x1000 that is not intercepted by the system message
// switch (LinkMsg, UnlinkMsg, MonitorMsg, etc.), so it reaches the behavior.

static void inject_system_message(EventBasedActor* actor) {
    inject_message(actor, TypeTag(0x07));
}

// ── Helper: get DLQ depth ────────────────────────────────────────────────────

static uint32_t dlq_depth(ActorSystem& system) {
    auto snapshot = system.dead_letter_snapshot();
    return snapshot.depth;
}

// ── Helper: spawn and downcast ───────────────────────────────────────────────

static DrainTestActor* spawn_test_actor(ActorSystem& system) {
    auto actor_ref = system.spawn<DrainTestActor>();
    auto actor_ptr = actor_ref.get(); // shared_ptr<AbstractActor>
    return static_cast<DrainTestActor*>(actor_ptr.get());
}

// ── Test 1: Drain policy processes all messages normally ─────────────────────

static void test_drain_policy_processes_all_messages() {
    Config cfg;
    cfg.scheduler_threads = 1;
    cfg.enable_network = false;
    ActorSystem system(cfg);
    auto* actor = spawn_test_actor(system);

    auto* lc = actor->as_lifecycle();
    assert(lc != nullptr);
    lc->set_drain_config(DrainConfig{DrainPolicy::Drain});

    // Enqueue 3 user messages + 1 system message
    inject_message(actor, TypeTag(0x1001)); // user
    inject_message(actor, TypeTag(0x1002)); // user
    inject_message(actor, TypeTag(0x1003)); // user
    inject_system_message(actor);           // system

    // Transition to kDraining
    bool ok = lc->transition(LifecycleState::kDraining);
    assert(ok);

    // Process messages via receive
    TypedMessage msg;
    auto* mailbox = actor->get_mailbox();
    while (mailbox->try_pop(msg)) {
        actor->receive(msg);
        // After each message, check if we should stop
        if (lc->state() == LifecycleState::kStopped)
            break;
    }

    // All 4 messages should be processed
    assert(actor->user_handler_count == 3);
    assert(actor->system_handler_count == 1);
    // Should have transitioned to Stopped
    assert(lc->state() == LifecycleState::kStopped);

    std::cout << "PASS: test_drain_policy_processes_all_messages\n";
}

// ── Test 2: DropUserMessages dead-letters user messages, keeps system ────────

static void test_drop_user_messages_deadletters_user_keeps_system() {
    Config cfg;
    cfg.scheduler_threads = 1;
    cfg.enable_network = false;
    ActorSystem system(cfg);
    auto* actor = spawn_test_actor(system);

    auto* lc = actor->as_lifecycle();
    assert(lc != nullptr);
    lc->set_drain_config(DrainConfig{DrainPolicy::DropUserMessages});

    // Enqueue 2 user messages + 1 LinkMsg
    inject_message(actor, TypeTag(0x1001)); // user
    inject_message(actor, TypeTag(0x1002)); // user
    inject_system_message(actor);           // system (LinkMsg)

    bool ok = lc->transition(LifecycleState::kDraining);
    assert(ok);

    // Process messages via receive
    TypedMessage msg;
    auto* mailbox = actor->get_mailbox();
    while (mailbox->try_pop(msg)) {
        actor->receive(msg);
        if (lc->state() == LifecycleState::kStopped)
            break;
    }

    // LinkMsg was processed, user messages went to DLQ
    assert(actor->system_handler_count == 1);
    assert(actor->user_handler_count == 0);
    assert(dlq_depth(system) == 2);

    std::cout << "PASS: "
                 "test_drop_user_messages_deadletters_user_keeps_system\n";
}

// ── Test 3: ImmediateStop dead-letters all messages ──────────────────────────

static void test_immediate_stop_deadletters_all() {
    Config cfg;
    cfg.scheduler_threads = 1;
    cfg.enable_network = false;
    ActorSystem system(cfg);
    auto* actor = spawn_test_actor(system);

    auto* lc = actor->as_lifecycle();
    assert(lc != nullptr);
    lc->set_drain_config(DrainConfig{DrainPolicy::ImmediateStop});

    // Enqueue 2 user + 1 system message
    inject_message(actor, TypeTag(0x1001)); // user
    inject_message(actor, TypeTag(0x1002)); // user
    inject_system_message(actor);           // system

    // Call drain_all_immediate()
    actor->drain_all_immediate();

    // All 3 should be dead-lettered, none processed
    assert(actor->user_handler_count == 0);
    assert(actor->system_handler_count == 0);
    assert(dlq_depth(system) == 3);

    std::cout << "PASS: test_immediate_stop_deadletters_all\n";
}

// ── Test 4: Deferred policy falls back to Drain ──────────────────────────────

static void test_deferred_policy_falls_back_to_drain() {
    Config cfg;
    cfg.scheduler_threads = 1;
    cfg.enable_network = false;
    ActorSystem system(cfg);
    auto* actor = spawn_test_actor(system);

    auto* lc = actor->as_lifecycle();
    assert(lc != nullptr);
    lc->set_drain_config(DrainConfig{DrainPolicy::SnapshotAndStop});

    // Enqueue 1 user message
    inject_message(actor, TypeTag(0x1001)); // user

    bool ok = lc->transition(LifecycleState::kDraining);
    assert(ok);

    // Process message via receive — drain_one should change policy to Drain
    TypedMessage msg;
    auto* mailbox = actor->get_mailbox();
    bool found = mailbox->try_pop(msg);
    assert(found);
    actor->receive(msg);

    // Policy should have been changed to Drain
    assert(lc->drain_config().policy == DrainPolicy::Drain);
    // Message should be processed normally
    assert(actor->user_handler_count == 1);
    assert(lc->state() == LifecycleState::kStopped);

    std::cout << "PASS: test_deferred_policy_falls_back_to_drain\n";
}

int main() {
    test_drain_policy_processes_all_messages();
    test_drop_user_messages_deadletters_user_keeps_system();
    test_immediate_stop_deadletters_all();
    test_deferred_policy_falls_back_to_drain();
    std::cout << "\nAll drain policy tests passed.\n";
    return 0;
}
