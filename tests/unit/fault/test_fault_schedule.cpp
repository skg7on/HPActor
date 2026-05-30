// Copyright 2026 HPActor Contributors
// Licensed under the Apache License, Version 2.0
#include <hpactor/fault/fault_schedule.hpp>

#include <gtest/gtest.h>

namespace hpactor::fault {
namespace {

TEST(FaultSchedule, EmptyByDefault) {
    FaultSchedule schedule;
    EXPECT_TRUE(schedule.empty());
    EXPECT_EQ(schedule.size(), 0u);
}

TEST(FaultSchedule, AddEntry) {
    FaultSchedule schedule;
    schedule.add_entry({FaultDomain::kMailbox, 0, "hpactor.mailbox.enqueue.fail",
                        FaultAction::kFail, std::nullopt, FailPayload{-1}});
    EXPECT_EQ(schedule.size(), 1u);
    const auto& e = schedule.entries()[0];
    EXPECT_EQ(e.domain, FaultDomain::kMailbox);
    EXPECT_EQ(e.at_tick, 0u);
    EXPECT_EQ(e.action, FaultAction::kFail);
}

TEST(FaultSchedule, MultipleEntries) {
    FaultSchedule schedule;
    schedule.add_entry({FaultDomain::kMailbox, 0, "hpactor.mailbox.enqueue.fail",
                        FaultAction::kFail, std::nullopt, FailPayload{-1}});
    schedule.add_entry({FaultDomain::kTransport, 42, "hpactor.transport.send.drop",
                        FaultAction::kDrop, std::nullopt, std::monostate{}});
    schedule.add_entry({FaultDomain::kAllocator, 5, "hpactor.allocator.oom",
                        FaultAction::kFail, std::nullopt, FailPayload{12}});
    EXPECT_EQ(schedule.size(), 3u);
}

TEST(FaultSchedule, Clear) {
    FaultSchedule schedule;
    schedule.add_entry({FaultDomain::kMailbox, 0, "hpactor.mailbox.enqueue.fail",
                        FaultAction::kFail, std::nullopt, FailPayload{-1}});
    schedule.clear();
    EXPECT_TRUE(schedule.empty());
}

TEST(FaultSchedule, EntriesPreserveOrder) {
    FaultSchedule schedule;
    for (int i = 0; i < 10; ++i) {
        schedule.add_entry({FaultDomain::kTransport, static_cast<uint64_t>(i),
                            "hpactor.transport.send.drop", FaultAction::kDrop,
                            std::nullopt, std::monostate{}});
    }
    EXPECT_EQ(schedule.size(), 10u);
    for (size_t i = 0; i < 10; ++i) {
        EXPECT_EQ(schedule.entries()[i].at_tick, static_cast<uint64_t>(i));
    }
}

TEST(FaultSchedule, AllFiveActions) {
    FaultSchedule schedule;
    schedule.add_entry({FaultDomain::kMailbox, 1, "a", FaultAction::kFail,
                        std::nullopt, FailPayload{1}});
    schedule.add_entry({FaultDomain::kTransport, 2, "b", FaultAction::kDrop,
                        std::nullopt, std::monostate{}});
    schedule.add_entry({FaultDomain::kScheduler, 3, "c", FaultAction::kDelay,
                        std::nullopt, DelayPayload{5}});
    schedule.add_entry({FaultDomain::kAllocator, 4, "d", FaultAction::kCorrupt,
                        std::nullopt, CorruptPayload{0, 0xFF}});
    schedule.add_entry({FaultDomain::kActor, 5, "e", FaultAction::kPanic,
                        std::nullopt, std::monostate{}});
    EXPECT_EQ(schedule.size(), 5u);
}

TEST(FaultSchedule, WithTarget) {
    FaultSchedule schedule;
    ActorId target(42);
    schedule.add_entry({FaultDomain::kMailbox, 0, "hpactor.mailbox.enqueue.fail",
                        FaultAction::kFail, target, FailPayload{-1}});
    const auto& opt = schedule.entries()[0].target;
    ASSERT_TRUE(opt.has_value());
    EXPECT_EQ(*opt, target); // NOLINT(bugprone-unchecked-optional-access)
}

TEST(FaultSchedule, ExpandRandomGeneratesEntriesAtProbability) {
    FaultSchedule schedule;
    std::mt19937 rng(42);

    schedule.expand_random(FaultDomain::kTransport,
                           "hpactor.transport.send.drop",
                           FaultAction::kDrop,
                           /*probability=*/0.1,
                           /*max_ticks=*/1000,
                           rng);

    size_t count = schedule.size();
    EXPECT_GT(count, 50);
    EXPECT_LT(count, 150);

    for (const auto& entry : schedule.entries()) {
        EXPECT_EQ(entry.domain, FaultDomain::kTransport);
        EXPECT_EQ(entry.path, "hpactor.transport.send.drop");
        EXPECT_EQ(entry.action, FaultAction::kDrop);
        EXPECT_LT(entry.at_tick, 1000);
    }
}

TEST(FaultSchedule, ExpandRandomDeterministic) {
    FaultSchedule s1, s2;
    std::mt19937 rng1(12345);
    std::mt19937 rng2(12345);

    s1.expand_random(FaultDomain::kTransport, "hpactor.transport.send.drop",
                     FaultAction::kDrop, 0.05, 500, rng1);
    s2.expand_random(FaultDomain::kTransport, "hpactor.transport.send.drop",
                     FaultAction::kDrop, 0.05, 500, rng2);

    EXPECT_EQ(s1.size(), s2.size());
    for (size_t i = 0; i < s1.size(); ++i) {
        EXPECT_EQ(s1.entries()[i].at_tick, s2.entries()[i].at_tick);
    }
}

TEST(FaultSchedule, SortOrdersByDomainThenTick) {
    FaultSchedule schedule;

    schedule.add_entry(FaultScheduleEntry{
        FaultDomain::kTransport, 5, "b", FaultAction::kDrop, {}, {}});
    schedule.add_entry(FaultScheduleEntry{
        FaultDomain::kMailbox, 10, "a", FaultAction::kFail, {}, {}});
    schedule.add_entry(FaultScheduleEntry{
        FaultDomain::kMailbox, 3, "c", FaultAction::kDrop, {}, {}});

    schedule.sort();

    const auto& entries = schedule.entries();
    EXPECT_EQ(entries[0].domain, FaultDomain::kMailbox);
    EXPECT_EQ(entries[0].at_tick, 3);
    EXPECT_EQ(entries[1].domain, FaultDomain::kMailbox);
    EXPECT_EQ(entries[1].at_tick, 10);
    EXPECT_EQ(entries[2].domain, FaultDomain::kTransport);
    EXPECT_EQ(entries[2].at_tick, 5);
}

} // anonymous namespace
} // namespace hpactor::fault
