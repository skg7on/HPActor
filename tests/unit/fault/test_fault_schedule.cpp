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

} // anonymous namespace
} // namespace hpactor::fault
