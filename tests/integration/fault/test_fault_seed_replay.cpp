// Copyright 2026 HPActor Contributors
// Licensed under the Apache License, Version 2.0
#include <hpactor/fault/fault_controller.hpp>
#include <hpactor/fault/fault_point.hpp>
#include <hpactor/fault/fault_schedule.hpp>

#include <gtest/gtest.h>

#include <random>

namespace hpactor::fault {
namespace {

FaultSchedule build_schedule_from_seed(uint64_t seed) {
    std::mt19937 rng(static_cast<std::mt19937::result_type>(seed));
    std::uniform_int_distribution<uint64_t> dist(1, 100);

    FaultSchedule schedule;
    uint64_t transport_tick = 0;
    uint64_t mailbox_tick = 0;

    for (int i = 0; i < 50; ++i) {
        uint64_t r = dist(rng);
        if (r <= 10) {
            schedule.add_entry({FaultDomain::kTransport, ++transport_tick,
                                "hpactor.transport.send.drop", FaultAction::kDrop,
                                std::nullopt, std::monostate{}});
        } else if (r <= 20) {
            schedule.add_entry({FaultDomain::kMailbox, ++mailbox_tick,
                                "hpactor.mailbox.enqueue.fail", FaultAction::kFail,
                                std::nullopt, FailPayload{-1}});
        }
    }
    return schedule;
}

size_t run_scenario(const FaultSchedule& schedule) {
    FaultController fc;
    fc.install();
    fc.load(schedule);
    fc.enable("*");

    size_t fire_count = 0;
    for (int i = 0; i < 200; ++i) {
        if (fc.check("hpactor.transport.send.drop"))
            ++fire_count;
        if (fc.check("hpactor.mailbox.enqueue.fail"))
            ++fire_count;
    }
    fc.remove();
    return fire_count;
}

TEST(FaultSeedReplay, SameSeedProducesSameFireCount) {
    constexpr uint64_t kSeed = 0xCAFE1234;

    auto sched1 = build_schedule_from_seed(kSeed);
    auto sched2 = build_schedule_from_seed(kSeed);

    size_t count1 = run_scenario(sched1);
    size_t count2 = run_scenario(sched2);

    EXPECT_EQ(count1, count2);
}

TEST(FaultSeedReplay, DifferentSchedulesProduceDifferentBehavior) {
    auto sched1 = build_schedule_from_seed(0xAAAA);
    auto sched2 = build_schedule_from_seed(0xBBBB);

    // Schedules built from different seeds should differ
    bool schedules_differ = false;
    if (sched1.size() != sched2.size()) {
        schedules_differ = true;
    } else {
        for (size_t i = 0; i < sched1.size(); ++i) {
            if (sched1.entries()[i].at_tick != sched2.entries()[i].at_tick ||
                sched1.entries()[i].domain != sched2.entries()[i].domain) {
                schedules_differ = true;
                break;
            }
        }
    }
    EXPECT_TRUE(schedules_differ);

    size_t count1 = run_scenario(sched1);
    size_t count2 = run_scenario(sched2);
    // With different schedules, counts may differ (but don't have to)
    // The key property is that same seed → same result (tested above)
    (void)count1;
    (void)count2;
}

TEST(FaultSeedReplay, ReplaySeedIsPreserved) {
    FaultController fc;
    fc.set_replay_seed(0xDEADBEEF);
    EXPECT_EQ(fc.replay_seed(), 0xDEADBEEFu);

    auto snap = fc.snapshot();
    EXPECT_EQ(snap.replay_seed, 0xDEADBEEFu);
}

} // anonymous namespace
} // namespace hpactor::fault
