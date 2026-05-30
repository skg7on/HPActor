// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#include <hpactor/fault/fault_controller.hpp>
#include <hpactor/fault/fault_macros.hpp>
#include <hpactor/fault/fault_schedule.hpp>

#include <gtest/gtest.h>

namespace hpactor::fault {
namespace {

TEST(FaultMacro, MacroNoOpWhenDisabled) {
    FaultController fc;
    fc.install();

    bool fault_fired = false;
    FAULT_INJECT("hpactor.mailbox.enqueue.fail") {
        fault_fired = true;
    }
    EXPECT_FALSE(fault_fired);

    fc.remove();
}

TEST(FaultMacro, MacroNoOpWhenNotInstalled) {
    bool fault_fired = false;
    FAULT_INJECT("hpactor.mailbox.enqueue.fail") {
        fault_fired = true;
    }
    EXPECT_FALSE(fault_fired);
}

TEST(FaultMacro, MacroFiresWhenScheduled) {
    FaultController fc;
    fc.install();

    FaultSchedule schedule;
    add_entry_to(schedule, FaultDomain::kMailbox, 1)
        .fail("hpactor.mailbox.enqueue.fail", 1);

    fc.load(schedule);
    fc.enable("*");

    bool fault_fired = false;
    FAULT_INJECT("hpactor.mailbox.enqueue.fail") {
        fault_fired = true;
    }
    EXPECT_TRUE(fault_fired);
    EXPECT_EQ(fc.faults_fired(), 1);

    fc.remove();
}

TEST(FaultMacro, MacroDoesNotFireForDifferentPath) {
    FaultController fc;
    fc.install();

    FaultSchedule schedule;
    add_entry_to(schedule, FaultDomain::kTransport, 0)
        .drop("hpactor.transport.send.drop");

    fc.load(schedule);
    fc.enable("*");

    bool fault_fired = false;
    FAULT_INJECT("hpactor.mailbox.enqueue.fail") {
        fault_fired = true;
    }
    EXPECT_FALSE(fault_fired);

    fc.remove();
}

TEST(FaultMacro, MacroOnlyFiresAtCorrectTick) {
    FaultController fc;
    fc.install();

    FaultSchedule schedule;
    add_entry_to(schedule, FaultDomain::kMailbox, 5)
        .fail("hpactor.mailbox.enqueue.fail", 1);

    fc.load(schedule);
    fc.enable("*");

    // tick 0→1 on 1st call, 1→2 on 2nd, ..., 4→5 on 5th
    for (int i = 0; i < 4; i++) {
        bool fault_fired = false;
        FAULT_INJECT("hpactor.mailbox.enqueue.fail") {
            fault_fired = true;
        }
        EXPECT_FALSE(fault_fired) << "should not fire at check " << i;
    }

    // 5th call: tick 4→5, matches at_tick=5
    bool fault_fired = false;
    FAULT_INJECT("hpactor.mailbox.enqueue.fail") {
        fault_fired = true;
    }
    EXPECT_TRUE(fault_fired);

    fc.remove();
}

} // anonymous namespace
} // namespace hpactor::fault
