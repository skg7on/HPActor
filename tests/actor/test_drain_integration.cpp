// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#include <hpactor/actor/abstract_actor.hpp>
#include <hpactor/actor/drain_config.hpp>
#include <hpactor/actor/drain_policy.hpp>
#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/actor/lifecycle_actor.hpp>
#include <hpactor/actor/lifecycle_state.hpp>
#include <hpactor/actor/typed_message.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/mailbox/dead_letter_queue.hpp>
#include <hpactor/mem/memory_config.hpp>
#include <hpactor/types/types.hpp>

#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

using namespace hpactor;

// ── Integration test actor with lifecycle and counting handlers ─────────────

class IntegrationTestActor : public EventBasedActor, public LifecycleActor {
  public:
    int user_handler_count = 0;
    int system_handler_count = 0;

    IntegrationTestActor(ActorContext* ctx, ActorSystem& sys)
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
        return Behavior{[this](TypedMessage& msg) {
            if (static_cast<uint32_t>(msg.type_id()) < 0x1000) {
                system_handler_count++;
            } else {
                user_handler_count++;
            }
        }};
    }
};

// ── Helper: inject a test message into an actor's mailbox ──────────────────

static void inject_message(EventBasedActor* actor, TypeTag tag) {
    auto* mailbox = actor->get_mailbox();
    assert(mailbox != nullptr);

    static_assert(sizeof(TypedMessage) <= 1024, "TypedMessage must fit in 1024 "
                                                "bytes for stack allocation");

    // Allocate a TypedMessage node via the custom allocator
    auto* node = static_cast<TypedMessage*>(mem::allocate(
        mem::RegionType::kMessage, sizeof(TypedMessage), actor->id()));
    new (node) TypedMessage(tag, StreamBuffer{});
    node->set_sender_address(ActorAddress{});

    mailbox->inject_for_test(node);
}

// ── Helper: enqueue a system message (bypasses system message switch) ──────

static void inject_system_message(EventBasedActor* actor) {
    inject_message(actor, TypeTag(0x07));
}

// ── Helper: get DLQ depth ──────────────────────────────────────────────────

static uint32_t dlq_depth(ActorSystem& system) {
    return system.dead_letter_snapshot().depth;
}

// ── DLQ counts bucket (used to collect per-actor reason counts in one pass) ─

struct DlqCounts {
    uint32_t total = 0;
    uint32_t drain_policy_drop = 0;
    uint32_t mailbox_closed = 0;
    uint32_t drain_timeout = 0;
    uint32_t other = 0;
};

// ═══════════════════════════════════════════════════════════════════════════════
// Test 1: full_shutdown_drains_spawn_tree
// Spawns 4 actors (1 parent + 3 children), enqueues messages, calls shutdown(),
// and verifies all actors reach kStopped and messages land in DLQ.
// ═══════════════════════════════════════════════════════════════════════════════

static void test_full_shutdown_drains_spawn_tree() {
    Config cfg;
    cfg.scheduler_threads = 0;
    cfg.enable_network = false;
    ActorSystem system(cfg);

    // Spawn 4 actors (conceptually 1 parent + 3 children)
    auto ref1 = system.spawn<IntegrationTestActor>();
    auto ref2 = system.spawn<IntegrationTestActor>();
    auto ref3 = system.spawn<IntegrationTestActor>();
    auto ref4 = system.spawn<IntegrationTestActor>();

    auto* a1 = static_cast<IntegrationTestActor*>(ref1.get().get());
    auto* a2 = static_cast<IntegrationTestActor*>(ref2.get().get());
    auto* a3 = static_cast<IntegrationTestActor*>(ref3.get().get());
    auto* a4 = static_cast<IntegrationTestActor*>(ref4.get().get());

    // Use ImmediateStop for deterministic shutdown
    a1->as_lifecycle()->set_drain_config(DrainConfig{DrainPolicy::ImmediateStop});
    a2->as_lifecycle()->set_drain_config(DrainConfig{DrainPolicy::ImmediateStop});
    a3->as_lifecycle()->set_drain_config(DrainConfig{DrainPolicy::ImmediateStop});
    a4->as_lifecycle()->set_drain_config(DrainConfig{DrainPolicy::ImmediateStop});

    // Enqueue 3 user messages to each actor
    for (auto* actor : {a1, a2, a3, a4}) {
        inject_message(actor, TypeTag(0x1001));
        inject_message(actor, TypeTag(0x1002));
        inject_message(actor, TypeTag(0x1003));
    }

    // Call shutdown
    ShutdownOptions opts;
    opts.ingress_timeout = std::chrono::milliseconds(10);
    opts.actor_drain_timeout = std::chrono::milliseconds(10);
    opts.cluster_leave_timeout = std::chrono::milliseconds(10);

    auto result = system.shutdown(opts);
    assert(result.has_value());

    // All 4 actors should reach kStopped
    for (auto* actor : {a1, a2, a3, a4}) {
        assert(actor->as_lifecycle()->state() == LifecycleState::kStopped);
    }

    // All 12 messages should be in DLQ (ImmediateStop dead-letters everything)
    assert(dlq_depth(system) == 12);

    // Pop all DLQ records and verify they are all MailboxClosed.
    // Collect all target actor IDs to verify coverage.
    std::vector<ActorId> dlq_targets;
    {
        mailbox::DeadLetterRecord record;
        while (system.pop_dead_letter(record)) {
            assert(record.reason == mailbox::DeadLetterReason::MailboxClosed);
            dlq_targets.push_back(record.target.id);
        }
    }
    assert(dlq_targets.size() == 12);
    // Verify each of the 4 actors appears exactly 3 times
    int a1_hits = 0, a2_hits = 0, a3_hits = 0, a4_hits = 0;
    for (auto& target_id : dlq_targets) {
        if (target_id == a1->id())
            a1_hits++;
        else if (target_id == a2->id())
            a2_hits++;
        else if (target_id == a3->id())
            a3_hits++;
        else if (target_id == a4->id())
            a4_hits++;
    }
    assert(a1_hits == 3);
    assert(a2_hits == 3);
    assert(a3_hits == 3);
    assert(a4_hits == 3);

    std::cout << "PASS: test_full_shutdown_drains_spawn_tree\n";
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 2: drain_policy_flows_end_to_end
// Spawns 3 actors with different DrainPolicy values.  Actors A (Drain) and B
// (DropUserMessages) are manually transitioned to kDraining and drained —
// exercising the per-message drain_one() logic which simulates what the
// scheduler does during a live drain.  Actor C (ImmediateStop) is exercised
// through ActorSystem::shutdown(), which calls drain_all_immediate()
// synchronously and transitions directly to kStopped.
// ═══════════════════════════════════════════════════════════════════════════════

static void test_drain_policy_flows_end_to_end() {
    Config cfg;
    cfg.scheduler_threads = 1;
    cfg.enable_network = false;
    ActorSystem system(cfg);

    // ── Spawn 3 actors with different drain policies ──────────────────────
    auto ref_a = system.spawn<IntegrationTestActor>();
    auto* actor_a = static_cast<IntegrationTestActor*>(ref_a.get().get());
    actor_a->as_lifecycle()->set_drain_config(
        DrainConfig{DrainPolicy::Drain, std::chrono::milliseconds{500}});

    auto ref_b = system.spawn<IntegrationTestActor>();
    auto* actor_b = static_cast<IntegrationTestActor*>(ref_b.get().get());
    actor_b->as_lifecycle()->set_drain_config(DrainConfig{
        DrainPolicy::DropUserMessages, std::chrono::milliseconds{500}});

    auto ref_c = system.spawn<IntegrationTestActor>();
    auto* actor_c = static_cast<IntegrationTestActor*>(ref_c.get().get());
    actor_c->as_lifecycle()->set_drain_config(
        DrainConfig{DrainPolicy::ImmediateStop});

    // ── Enqueue messages ──────────────────────────────────────────────────
    // Actor A: 3 user messages (all should be processed by Drain policy)
    inject_message(actor_a, TypeTag(0x1001));
    inject_message(actor_a, TypeTag(0x1002));
    inject_message(actor_a, TypeTag(0x1003));

    // Actor B: 2 user + 2 system messages
    //   user messages -> dead-lettered with DrainPolicyDrop
    //   system messages -> processed normally
    inject_message(actor_b, TypeTag(0x1001));
    inject_message(actor_b, TypeTag(0x1002));
    inject_system_message(actor_b);
    inject_system_message(actor_b);

    // Actor C: 2 user + 1 system messages (all -> DLQ via ImmediateStop)
    inject_message(actor_c, TypeTag(0x1001));
    inject_message(actor_c, TypeTag(0x1002));
    inject_system_message(actor_c);

    // ── Manually drain actor A (Drain policy) ─────────────────────────────
    {
        bool ok = actor_a->as_lifecycle()->transition(LifecycleState::kDraining);
        assert(ok);
        TypedMessage msg;
        auto* mailbox = actor_a->get_mailbox();
        while (mailbox->try_pop(msg)) {
            actor_a->receive(msg);
            if (actor_a->as_lifecycle()->state() == LifecycleState::kStopped)
                break;
        }
    }

    // ── Manually drain actor B (DropUserMessages policy) ──────────────────
    {
        bool ok = actor_b->as_lifecycle()->transition(LifecycleState::kDraining);
        assert(ok);
        TypedMessage msg;
        auto* mailbox = actor_b->get_mailbox();
        while (mailbox->try_pop(msg)) {
            actor_b->receive(msg);
            if (actor_b->as_lifecycle()->state() == LifecycleState::kStopped)
                break;
        }
    }

    // ── Verify per-actor handler counts before shutdown ───────────────────
    // Actor A (Drain): all 3 user messages processed
    assert(actor_a->user_handler_count == 3);
    assert(actor_a->system_handler_count == 0);

    // Actor B (DropUserMessages): 2 system messages processed,
    // 2 user messages dead-lettered
    assert(actor_b->system_handler_count == 2);
    assert(actor_b->user_handler_count == 0);

    // Actor C: not yet drained; messages still in mailbox
    assert(actor_c->user_handler_count == 0);
    assert(actor_c->system_handler_count == 0);

    // ── Call shutdown — actor C (ImmediateStop) is handled synchronously ──
    ShutdownOptions opts;
    opts.ingress_timeout = std::chrono::milliseconds(10);
    opts.actor_drain_timeout = std::chrono::milliseconds(10);
    opts.cluster_leave_timeout = std::chrono::milliseconds(10);

    auto result = system.shutdown(opts);
    assert(result.has_value());

    // ── All 3 actors must reach kStopped ──────────────────────────────────
    assert(actor_a->as_lifecycle()->state() == LifecycleState::kStopped);
    assert(actor_b->as_lifecycle()->state() == LifecycleState::kStopped);
    assert(actor_c->as_lifecycle()->state() == LifecycleState::kStopped);

    // Actor C must not have processed any messages (ImmediateStop)
    assert(actor_c->user_handler_count == 0);
    assert(actor_c->system_handler_count == 0);

    // ── Pop all DLQ records in a single pass and bucket by actor + reason ─
    DlqCounts counts_a;
    DlqCounts counts_b;
    DlqCounts counts_c;
    {
        mailbox::DeadLetterRecord record;
        while (system.pop_dead_letter(record)) {
            DlqCounts* bucket = nullptr;
            if (record.target.id == actor_a->id())
                bucket = &counts_a;
            else if (record.target.id == actor_b->id())
                bucket = &counts_b;
            else if (record.target.id == actor_c->id())
                bucket = &counts_c;
            else
                continue;
            bucket->total++;
            switch (record.reason) {
                case mailbox::DeadLetterReason::DrainPolicyDrop:
                    bucket->drain_policy_drop++;
                    break;
                case mailbox::DeadLetterReason::MailboxClosed:
                    bucket->mailbox_closed++;
                    break;
                case mailbox::DeadLetterReason::DrainTimeout:
                    bucket->drain_timeout++;
                    break;
                default:
                    bucket->other++;
                    break;
            }
        }
    }

    // Actor A (Drain): no records in DLQ — all messages processed normally
    assert(counts_a.total == 0);

    // Actor B (DropUserMessages): 2 user messages dead-lettered with
    // DrainPolicyDrop reason, 0 with MailboxClosed (system messages processed)
    assert(counts_b.total == 2);
    assert(counts_b.drain_policy_drop == 2);
    assert(counts_b.mailbox_closed == 0);

    // Actor C (ImmediateStop): all 3 messages dead-lettered with MailboxClosed
    assert(counts_c.total == 3);
    assert(counts_c.mailbox_closed == 3);

    std::cout << "PASS: test_drain_policy_flows_end_to_end\n";
}

int main() {
    test_full_shutdown_drains_spawn_tree();
    test_drain_policy_flows_end_to_end();
    std::cout << "\nAll drain integration tests passed.\n";
    return 0;
}
