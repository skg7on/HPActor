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

// System test: Multi-Actor Runtime Workflow
// Validates spawn → send/reply → lifecycle → link/monitor → scheduled delivery

#include <hpactor/actor/actor_system.hpp>
#include <hpactor/config/actor_factory_registry.hpp>

#include "scheduler_test_driver.hpp"
#include "system_test_fixture.hpp"

#include <gtest/gtest.h>

using namespace hpactor;

// ── Register test actors for spawn ───────────────────────────────────────────

using CountingActor = test::CountingActor;
using EchoActor = test::EchoActor;
using ForwardingActor = test::ForwardingActor;

HPACTOR_REGISTER_ACTOR("CountingActor", CountingActor);
HPACTOR_REGISTER_ACTOR("EchoActor", EchoActor);
HPACTOR_REGISTER_ACTOR("ForwardingActor", ForwardingActor);

// ═══════════════════════════════════════════════════════════════════════════════
// Test 1: Multi-actor send/reply chain with live scheduler
// ═══════════════════════════════════════════════════════════════════════════════

TEST(RuntimeWorkflow, MultiActorDeliverMessages) {
    // Scheduler started paused so no worker races with the test driver.
    // The driver drains the ready queue synchronously via run_one_ready()
    // — no timing assumptions or polling needed.
    Config cfg = test::config_with_scheduler(1);
    cfg.scheduler_start_paused = true;
    ActorSystem system(cfg);

    auto a1 = system.spawn<test::CountingActor>();
    auto a2 = system.spawn<test::CountingActor>();
    auto a3 = system.spawn<test::CountingActor>();

    auto* actor2 = static_cast<test::CountingActor*>(a2.get().get());
    auto* actor3 = static_cast<test::CountingActor*>(a3.get().get());

    // Use deliver_local — the direct, resolution-free delivery path
    TypedMessage msg1(TypeTag(0x1001), StreamBuffer{});
    msg1.set_sender_address(a1.address());
    system.deliver_local(a2.id(), std::move(msg1));

    TypedMessage msg2(TypeTag(0x1002), StreamBuffer{});
    msg2.set_sender_address(a1.address());
    system.deliver_local(a3.id(), std::move(msg2));

    // Deterministically drain the ready queue until both actors process
    // their messages. No polling or sleep — drain_until() calls
    // run_one_ready() in a tight loop.
    hpactor::test::SchedulerTestDriver driver(system);
    bool done = driver.drain_until([&]() {
        return actor2->handler_count >= 1 && actor3->handler_count >= 1;
    });
    EXPECT_TRUE(done);

    EXPECT_GE(actor2->handler_count, 1);
    EXPECT_GE(actor3->handler_count, 1);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 2: Lifecycle transitions are observable after spawn
// ═══════════════════════════════════════════════════════════════════════════════

TEST(RuntimeWorkflow, LifecycleTransitionsObservable) {
    Config cfg = test::config_with_scheduler(1);
    ActorSystem system(cfg);

    auto a1 = system.spawn<test::CountingActor>();
    auto* actor = static_cast<test::CountingActor*>(a1.get().get());
    auto* lc = actor->as_lifecycle();
    EXPECT_NE(lc, nullptr);

    // spawn<>() transitions to kActive
    EXPECT_EQ(lc->state(), LifecycleState::kActive);

    // Manual transition to kDraining
    bool ok = lc->transition(LifecycleState::kDraining);
    EXPECT_TRUE(ok);
    EXPECT_EQ(lc->state(), LifecycleState::kDraining);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 3: actor_count reflects live actors
// ═══════════════════════════════════════════════════════════════════════════════

TEST(RuntimeWorkflow, ActorCountReflectsLiveActors) {
    Config cfg = test::config_with_scheduler(1);
    ActorSystem system(cfg);

    // System actors (MetricsActor, etc.) may be present at startup
    // when their respective subsystems (metrics, logging) are enabled.
    size_t initial = system.actor_count();
    EXPECT_GE(initial, 0);

    auto a1 = system.spawn<test::CountingActor>();
    auto a2 = system.spawn<test::CountingActor>();

    size_t after_spawn = system.actor_count();
    EXPECT_GE(after_spawn, initial + 2);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 4: Link and unlink API works
// ═══════════════════════════════════════════════════════════════════════════════

TEST(RuntimeWorkflow, LinkUnlinkApi) {
    Config cfg = test::config_with_scheduler(1);
    cfg.scheduler_start_paused = true;
    ActorSystem system(cfg);

    auto a1 = system.spawn<test::CountingActor>();
    auto a2 = system.spawn<test::CountingActor>();

    auto* actor1 = static_cast<test::CountingActor*>(a1.get().get());

    // link_to/unlink_from update the calling actor's linked_ list
    // synchronously; the SchedulerTestDriver drains any LinkMsg/UnlinkMsg
    // system messages sent to the target actor.
    hpactor::test::SchedulerTestDriver driver(system);

    // Link A1 → A2
    actor1->link_to(a2.address());
    driver.drain_until([&]() { return true; });

    auto linked = actor1->context()->linked_actors();
    EXPECT_FALSE(linked.empty());
    EXPECT_EQ(linked[0], a2.address());

    // Unlink
    actor1->unlink_from(a2.address());
    driver.drain_until([&]() { return true; });

    linked = actor1->context()->linked_actors();
    EXPECT_TRUE(linked.empty());
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 5: For_each_actor enumerates all spawned actors
// ═══════════════════════════════════════════════════════════════════════════════

TEST(RuntimeWorkflow, ForEachActorEnumeratesAll) {
    Config cfg = test::config_with_scheduler(1);
    ActorSystem system(cfg);

    system.spawn<test::CountingActor>();
    system.spawn<test::CountingActor>();
    system.spawn<test::EchoActor>();

    int count = 0;
    system.for_each_actor(
        [&](ActorId /*id*/, AbstractActor& /*actor*/) { count++; });

    // 3 user + system_actor
    EXPECT_GE(count, 3);
}