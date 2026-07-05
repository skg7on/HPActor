// Copyright 2026 HPActor Contributors
// Licensed under the Apache License, Version 2.0

#include <hpactor/actor/system/actor_system.hpp>
#include <hpactor/fault/fault_controller.hpp>
#include <hpactor/fault/fault_macros.hpp>
#include <hpactor/fault/fault_point.hpp>
#include <hpactor/fault/fault_schedule.hpp>
#include <hpactor/fault/fault_types.hpp>
#include <hpactor/hpactor_config.hpp>

#include <gtest/gtest.h>

#include <random>
#include <string>

namespace hpactor::fault {
namespace {

// Helper: create an ActorSystem with no worker threads for deterministic
// testing
inline ::hpactor::ActorSystem make_test_system() {
    ::hpactor::Config cfg;
    cfg.scheduler_threads = 0;
    return ::hpactor::ActorSystem(cfg);
}

// ============================================================================
// FaultController lifecycle
// ============================================================================

TEST(FaultControllerIntegrationTest, DefaultConstructionIsDisabled) {
    auto system = make_test_system();
    auto& fc = system.fault_controller();
    EXPECT_FALSE(fc.is_enabled());
    EXPECT_EQ(fc.faults_fired(), 0u);
}

TEST(FaultControllerIntegrationTest, EnableDisable) {
    auto system = make_test_system();
    auto& fc = system.fault_controller();
    fc.enable("*");
    EXPECT_TRUE(fc.is_enabled());

    fc.disable();
    EXPECT_FALSE(fc.is_enabled());
}

TEST(FaultControllerIntegrationTest, LoadScheduleThenFires) {
    auto system = make_test_system();

    FaultSchedule schedule;
    schedule.add_entry({FaultDomain::kMailbox, 1, "hpactor.mailbox.enqueue.fail",
                        FaultAction::kFail, std::nullopt, FailPayload{-1}});

    auto& fc = system.fault_controller();
    fc.load(schedule);
    fc.enable("*");

    // FAULT_INJECT auto-advances the domain tick and checks schedule
    bool injected = false;
    FAULT_INJECT("hpactor.mailbox.enqueue.fail") {
        injected = true;
    }
    EXPECT_TRUE(injected);
    EXPECT_EQ(fc.faults_fired(), 1u);
}

TEST(FaultControllerIntegrationTest, ClearScheduleStopsInjection) {
    auto system = make_test_system();

    FaultSchedule schedule;
    schedule.add_entry({FaultDomain::kMailbox, 1, "hpactor.mailbox.enqueue.fail",
                        FaultAction::kFail, std::nullopt, FailPayload{-1}});

    auto& fc = system.fault_controller();
    fc.load(schedule);
    fc.enable("*");
    fc.clear();

    bool injected = false;
    FAULT_INJECT("hpactor.mailbox.enqueue.fail") {
        injected = true;
    }
    EXPECT_FALSE(injected);
    EXPECT_EQ(fc.faults_fired(), 0u);
}

TEST(FaultControllerIntegrationTest, ReplaySeed) {
    auto system = make_test_system();
    auto& fc = system.fault_controller();
    EXPECT_EQ(fc.replay_seed(), 0u);

    fc.set_replay_seed(12345);
    EXPECT_EQ(fc.replay_seed(), 12345u);
}

// ============================================================================
// FaultSchedule with multiple entries
// ============================================================================

TEST(FaultControllerIntegrationTest, FaultScheduleAddAndSort) {
    FaultSchedule schedule;
    EXPECT_EQ(schedule.size(), 0u);
    EXPECT_TRUE(schedule.empty());

    // Add entries out of order
    schedule.add_entry({FaultDomain::kMailbox, 3, "path.three",
                        FaultAction::kFail, std::nullopt, FailPayload{-1}});
    schedule.add_entry({FaultDomain::kMailbox, 1, "path.one",
                        FaultAction::kFail, std::nullopt, FailPayload{-1}});
    schedule.add_entry({FaultDomain::kMailbox, 2, "path.two",
                        FaultAction::kDrop, std::nullopt, std::monostate{}});

    EXPECT_EQ(schedule.size(), 3u);

    schedule.sort();

    const auto& entries = schedule.entries();
    EXPECT_EQ(entries[0].at_tick, 1u);
    EXPECT_EQ(entries[0].path, "path.one");
    EXPECT_EQ(entries[1].at_tick, 2u);
    EXPECT_EQ(entries[1].path, "path.two");
    EXPECT_EQ(entries[2].at_tick, 3u);
    EXPECT_EQ(entries[2].path, "path.three");
}

TEST(FaultControllerIntegrationTest, FaultScheduleBuilder) {
    FaultSchedule schedule;

    add_entry_to(schedule, FaultDomain::kTransport, 5)
        .fail("hpactor.transport.send.fail", -1)
        .drop("hpactor.transport.send.drop")
        .delay("hpactor.transport.send.delay", 10)
        .panic("hpactor.transport.send.panic");

    EXPECT_EQ(schedule.size(), 4u);
    schedule.sort();

    const auto& entries = schedule.entries();
    EXPECT_EQ(entries[0].action, FaultAction::kFail);
    EXPECT_EQ(entries[1].action, FaultAction::kDrop);
    EXPECT_EQ(entries[2].action, FaultAction::kDelay);
    EXPECT_EQ(entries[3].action, FaultAction::kPanic);
}

TEST(FaultControllerIntegrationTest, FaultScheduleWithTargetActor) {
    FaultSchedule schedule;
    ActorId target_actor(42);

    schedule.add_entry({FaultDomain::kActor, 1, "hpactor.actor.dispatch.fail",
                        FaultAction::kFail, target_actor, FailPayload{-1}});

    EXPECT_EQ(schedule.size(), 1u);
    const auto& entries = schedule.entries();
    ASSERT_TRUE(entries[0].target.has_value());
    EXPECT_EQ(entries[0].target->value(), 42u);
}

TEST(FaultControllerIntegrationTest, FaultScheduleClear) {
    FaultSchedule schedule;
    schedule.add_entry({FaultDomain::kMailbox, 1, "path", FaultAction::kFail,
                        std::nullopt, FailPayload{-1}});
    EXPECT_EQ(schedule.size(), 1u);

    schedule.clear();
    EXPECT_EQ(schedule.size(), 0u);
    EXPECT_TRUE(schedule.empty());
}

// ============================================================================
// FaultPoint registration
// ============================================================================

TEST(FaultControllerIntegrationTest, FaultPointRegistryRegistration) {
    auto& registry = FaultPointRegistry::instance();

    // Register a custom fault point
    registry.register_point("hpactor.test.custom.path", FaultDomain::kMailbox,
                            "Custom test fault point");

    const FaultPoint* pt = registry.lookup("hpactor.test.custom.path");
    ASSERT_NE(pt, nullptr);
    EXPECT_EQ(pt->path, "hpactor.test.custom.path");
    EXPECT_EQ(pt->domain, FaultDomain::kMailbox);
    EXPECT_EQ(pt->description, "Custom test fault point");
}

TEST(FaultControllerIntegrationTest, FaultPointPrefixMatching) {
    auto& registry = FaultPointRegistry::instance();

    registry.register_point("hpactor.test.alpha.one", FaultDomain::kMailbox,
                            "Alpha one");
    registry.register_point("hpactor.test.alpha.two", FaultDomain::kMailbox,
                            "Alpha two");
    registry.register_point("hpactor.test.beta", FaultDomain::kTransport, "Beta");

    // Wildcard matches everything
    EXPECT_TRUE(registry.matches_prefix("hpactor.test.alpha.one", "*"));
    EXPECT_TRUE(registry.matches_prefix("hpactor.test.beta", "*"));

    // Exact prefix
    EXPECT_TRUE(registry.matches_prefix("hpactor.test.alpha.one",
                                        "hpactor.test.alpha.one"));

    // Non-matching prefix
    EXPECT_FALSE(
        registry.matches_prefix("hpactor.test.alpha.one", "hpactor.transport"));

    // Collect by prefix — use wildcard suffix pattern
    std::vector<const FaultPoint*> results;
    registry.collect_prefix("hpactor.test.alpha.*", results);
    EXPECT_GE(results.size(), 2u);
}

// ============================================================================
// FaultAction dispatch
// ============================================================================

TEST(FaultControllerIntegrationTest, FaultActionToString) {
    EXPECT_EQ(to_string(FaultAction::kFail), "Fail");
    EXPECT_EQ(to_string(FaultAction::kDrop), "Drop");
    EXPECT_EQ(to_string(FaultAction::kDelay), "Delay");
    EXPECT_EQ(to_string(FaultAction::kCorrupt), "Corrupt");
    EXPECT_EQ(to_string(FaultAction::kPanic), "Panic");
}

TEST(FaultControllerIntegrationTest, FaultDomainToString) {
    EXPECT_EQ(to_string(FaultDomain::kMailbox), "kMailbox");
    EXPECT_EQ(to_string(FaultDomain::kTransport), "kTransport");
    EXPECT_EQ(to_string(FaultDomain::kScheduler), "kScheduler");
    EXPECT_EQ(to_string(FaultDomain::kGossip), "kGossip");
    EXPECT_EQ(to_string(FaultDomain::kActor), "kActor");
    EXPECT_EQ(to_string(FaultDomain::kRpc), "kRpc");
    EXPECT_EQ(to_string(FaultDomain::kMetrics), "kMetrics");
}

// ============================================================================
// Fault injection metrics
// ============================================================================

TEST(FaultControllerIntegrationTest, MultipleFaultsFired) {
    auto system = make_test_system();

    FaultSchedule schedule;
    schedule.add_entry({FaultDomain::kMailbox, 1, "hpactor.mailbox.enqueue.fail",
                        FaultAction::kFail, std::nullopt, FailPayload{-1}});
    schedule.add_entry({FaultDomain::kMailbox, 2, "hpactor.mailbox.enqueue.fail",
                        FaultAction::kFail, std::nullopt, FailPayload{-2}});

    auto& fc = system.fault_controller();
    fc.load(schedule);
    fc.enable("*");

    // First fault at tick 1 (check auto-advances to 1 then matches)
    bool inj1 = false;
    FAULT_INJECT("hpactor.mailbox.enqueue.fail") {
        inj1 = true;
    }
    EXPECT_TRUE(inj1);
    EXPECT_EQ(fc.faults_fired(), 1u);

    // Second fault at tick 2 (check auto-advances to 2 then matches)
    bool inj2 = false;
    FAULT_INJECT("hpactor.mailbox.enqueue.fail") {
        inj2 = true;
    }
    EXPECT_TRUE(inj2);
    EXPECT_EQ(fc.faults_fired(), 2u);
}

TEST(FaultControllerIntegrationTest, SnapshotReflectsState) {
    auto system = make_test_system();

    FaultSchedule schedule;
    schedule.add_entry({FaultDomain::kMailbox, 1, "hpactor.mailbox.enqueue.fail",
                        FaultAction::kFail, std::nullopt, FailPayload{-1}});

    auto& fc = system.fault_controller();
    fc.load(schedule);
    fc.enable("hpactor.mailbox.*");
    fc.set_replay_seed(999);

    FaultControllerSnapshot snap = fc.snapshot();
    EXPECT_TRUE(snap.enabled);
    EXPECT_EQ(snap.active_scope, "hpactor.mailbox.*");
    EXPECT_EQ(snap.replay_seed, 999u);
    EXPECT_EQ(snap.schedule_entry_count, 1u);
    EXPECT_EQ(snap.faults_fired, 0u);
}

// ============================================================================
// Fault seed replay determinism
// ============================================================================

TEST(FaultControllerIntegrationTest, ExpandRandomIsDeterministic) {
    FaultSchedule schedule1;
    FaultSchedule schedule2;

    // Use same seed for both schedules
    std::mt19937 rng1(42);
    std::mt19937 rng2(42);

    schedule1.expand_random(FaultDomain::kMailbox, "hpactor.mailbox.random.fail",
                            FaultAction::kFail, 0.1, 1000, rng1, FailPayload{-1});
    schedule2.expand_random(FaultDomain::kMailbox, "hpactor.mailbox.random.fail",
                            FaultAction::kFail, 0.1, 1000, rng2, FailPayload{-1});

    // Both schedules should have the same entries
    EXPECT_EQ(schedule1.size(), schedule2.size());
    EXPECT_EQ(schedule1.entries().size(), schedule2.entries().size());

    for (size_t i = 0; i < schedule1.size(); ++i) {
        EXPECT_EQ(schedule1.entries()[i].at_tick, schedule2.entries()[i].at_tick);
        EXPECT_EQ(schedule1.entries()[i].path, schedule2.entries()[i].path);
    }
}

TEST(FaultControllerIntegrationTest,
     ExpandRandomDifferentSeedsYieldDifferentResults) {
    FaultSchedule schedule1;
    FaultSchedule schedule2;

    std::mt19937 rng1(42);
    std::mt19937 rng2(99);

    schedule1.expand_random(FaultDomain::kMailbox, "hpactor.mailbox.random.fail",
                            FaultAction::kFail, 0.5, 100, rng1, FailPayload{-1});
    schedule2.expand_random(FaultDomain::kMailbox, "hpactor.mailbox.random.fail",
                            FaultAction::kFail, 0.5, 100, rng2, FailPayload{-1});

    // Different seeds should usually produce different schedules
    bool same = true;
    if (schedule1.size() != schedule2.size()) {
        same = false;
    } else {
        for (size_t i = 0; i < schedule1.size(); ++i) {
            if (schedule1.entries()[i].at_tick != schedule2.entries()[i].at_tick) {
                same = false;
                break;
            }
        }
    }
    // At p=0.5 over 100 ticks, statistically very unlikely to match
    EXPECT_FALSE(same);
}

} // anonymous namespace
} // namespace hpactor::fault
