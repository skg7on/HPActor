// Copyright 2026 HPActor Contributors
// Licensed under the Apache License, Version 2.0
#include <hpactor/fault/fault_controller.hpp>
#include <hpactor/fault/fault_point.hpp>

#include <gtest/gtest.h>

namespace hpactor::fault {
namespace {

class FaultControllerTest : public ::testing::Test {
  protected:
    void SetUp() override {
        fc_.install();
    }
    void TearDown() override {
        fc_.remove();
    }
    FaultController fc_;
};

TEST_F(FaultControllerTest, DisabledByDefault) {
    EXPECT_FALSE(fc_.is_enabled());
}

TEST_F(FaultControllerTest, CheckReturnsFalseWhenDisabled) {
    EXPECT_FALSE(fc_.check("hpactor.mailbox.enqueue.fail"));
    EXPECT_EQ(fc_.faults_fired(), 0u);
}

TEST_F(FaultControllerTest, EnableAndCheckWithMatchingSchedule) {
    FaultSchedule schedule;
    schedule.add_entry({FaultDomain::kMailbox, 1,
                        "hpactor.mailbox.enqueue.fail",
                        FaultAction::kFail, std::nullopt,
                        FailPayload{-1}});
    fc_.load(schedule);
    fc_.enable("*");

    // First check: tick advances 0→1, matches schedule at tick 1
    EXPECT_TRUE(fc_.check("hpactor.mailbox.enqueue.fail"));
    EXPECT_EQ(fc_.faults_fired(), 1u);
}

TEST_F(FaultControllerTest, CheckDoesNotRefire) {
    FaultSchedule schedule;
    schedule.add_entry({FaultDomain::kMailbox, 1,
                        "hpactor.mailbox.enqueue.fail",
                        FaultAction::kFail, std::nullopt,
                        FailPayload{-1}});
    fc_.load(schedule);
    fc_.enable("*");

    // First check fires at tick 1
    EXPECT_TRUE(fc_.check("hpactor.mailbox.enqueue.fail"));
    // Tick is now at 2 — no more matching entries
    EXPECT_FALSE(fc_.check("hpactor.mailbox.enqueue.fail"));
    EXPECT_EQ(fc_.faults_fired(), 1u);
}

TEST_F(FaultControllerTest, DomainTickIndependentAdvance) {
    FaultSchedule schedule;
    // Schedule entries are consumed in order. Check the lower-tick domain
    // first to ensure the cursor doesn't skip over earlier entries.
    schedule.add_entry({FaultDomain::kMailbox, 1,
                        "hpactor.mailbox.enqueue.fail",
                        FaultAction::kFail, std::nullopt,
                        FailPayload{-1}});
    schedule.add_entry({FaultDomain::kTransport, 3,
                        "hpactor.transport.send.drop",
                        FaultAction::kDrop, std::nullopt,
                        std::monostate{}});
    fc_.load(schedule);
    fc_.enable("*");

    // Mailbox fires at tick 1
    EXPECT_TRUE(fc_.check("hpactor.mailbox.enqueue.fail"));

    // Transport domain ticks independently: 1, 2... fires at tick 3
    EXPECT_FALSE(fc_.check("hpactor.transport.send.drop")); // tick 1
    EXPECT_FALSE(fc_.check("hpactor.transport.send.drop")); // tick 2
    EXPECT_TRUE(fc_.check("hpactor.transport.send.drop"));  // tick 3

    EXPECT_EQ(fc_.faults_fired(), 2u);
}

TEST_F(FaultControllerTest, ScopeFiltering) {
    FaultSchedule schedule;
    schedule.add_entry({FaultDomain::kMailbox, 1,
                        "hpactor.mailbox.enqueue.fail",
                        FaultAction::kFail, std::nullopt,
                        FailPayload{-1}});
    schedule.add_entry({FaultDomain::kTransport, 1,
                        "hpactor.transport.send.drop",
                        FaultAction::kDrop, std::nullopt,
                        std::monostate{}});
    fc_.load(schedule);
    fc_.enable("hpactor.transport.*");

    // Transport check SHOULD fire — in scope, tick 0→1 matches at_tick 1
    EXPECT_TRUE(fc_.check("hpactor.transport.send.drop"));
    EXPECT_EQ(fc_.faults_fired(), 1u);

    // Mailbox check should NOT fire — out of scope (but tick still advances)
    EXPECT_FALSE(fc_.check("hpactor.mailbox.enqueue.fail"));
    EXPECT_EQ(fc_.faults_fired(), 1u);
}

TEST_F(FaultControllerTest, ClearSchedule) {
    FaultSchedule schedule;
    schedule.add_entry({FaultDomain::kMailbox, 1,
                        "hpactor.mailbox.enqueue.fail",
                        FaultAction::kFail, std::nullopt,
                        FailPayload{-1}});
    fc_.load(schedule);
    fc_.enable("*");
    fc_.clear();

    EXPECT_FALSE(fc_.check("hpactor.mailbox.enqueue.fail"));
}

TEST_F(FaultControllerTest, Snapshot) {
    FaultSchedule schedule;
    schedule.add_entry({FaultDomain::kMailbox, 1,
                        "hpactor.mailbox.enqueue.fail",
                        FaultAction::kFail, std::nullopt,
                        FailPayload{-1}});
    fc_.load(schedule);
    fc_.enable("*");
    fc_.set_replay_seed(42);

    auto snap = fc_.snapshot();
    EXPECT_TRUE(snap.enabled);
    EXPECT_EQ(snap.replay_seed, 42u);
    EXPECT_EQ(snap.schedule_entry_count, 1u);
    EXPECT_EQ(snap.faults_fired, 0u);
}

TEST_F(FaultControllerTest, PerThreadInstall) {
    FaultController fc1;
    FaultController fc2;

    fc1.install();
    EXPECT_EQ(FaultController::instance(), &fc1);

    fc2.install();
    EXPECT_EQ(FaultController::instance(), &fc2);

    fc2.remove();
    EXPECT_EQ(FaultController::instance(), nullptr);

    fc1.remove();
}

TEST_F(FaultControllerTest, AggregateSnapshotSumsAcrossInstances) {
    FaultController fc1;
    FaultController fc2;

    FaultSchedule schedule;
    add_entry_to(schedule, FaultDomain::kMailbox, 1)
        .fail("hpactor.mailbox.enqueue.fail", 1);

    fc1.load(schedule);
    fc1.enable("*");
    fc1.install();

    fc2.load(schedule);
    fc2.enable("*");
    fc2.install();

    fc1.check("hpactor.mailbox.enqueue.fail");

    auto snap = FaultController::aggregate_snapshot();
    EXPECT_EQ(snap.faults_fired, 1);

    fc1.remove();
    fc2.remove();
}

TEST_F(FaultControllerTest, BroadcastLoadToAllInstances) {
    FaultController fc1;
    FaultController fc2;

    fc1.install();
    fc2.install();

    FaultSchedule schedule;
    add_entry_to(schedule, FaultDomain::kMailbox, 0)
        .fail("hpactor.mailbox.enqueue.fail", 1);

    fc1.load(schedule);
    fc1.enable("*");

    EXPECT_TRUE(fc1.is_enabled());
    EXPECT_TRUE(fc2.is_enabled());
    EXPECT_EQ(fc1.snapshot().schedule_entry_count, 1);
    EXPECT_EQ(fc2.snapshot().schedule_entry_count, 1);

    fc1.remove();
    fc2.remove();
}

TEST_F(FaultControllerTest, RemoveCleansUp) {
    FaultController fc;
    fc.install();
    EXPECT_EQ(FaultController::instance(), &fc);

    fc.remove();
    EXPECT_EQ(FaultController::instance(), nullptr);

    auto snap = FaultController::aggregate_snapshot();
    EXPECT_EQ(snap.faults_fired, 0);
}

} // anonymous namespace
} // namespace hpactor::fault
