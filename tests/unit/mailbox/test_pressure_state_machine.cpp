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

#include <hpactor/mailbox/detail/pressure_state_machine.hpp>

#include <gtest/gtest.h>

using namespace hpactor::mailbox;
using namespace hpactor::mailbox::detail;

class PressureStateMachineTest : public ::testing::Test {
  protected:
    PressureStateMachine psm;
    static constexpr double kHigh = 0.80;
    static constexpr double kLow = 0.50;
    static constexpr double kCritical = 1.00;
};

TEST_F(PressureStateMachineTest, StartsNormal) {
    EXPECT_EQ(psm.current_state(), MailboxPressureState::Normal);
}

TEST_F(PressureStateMachineTest, BelowLowWatermarkStaysNormal) {
    psm.update(0.30, false, kHigh, kLow, kCritical);
    EXPECT_EQ(psm.current_state(), MailboxPressureState::Normal);
}

TEST_F(PressureStateMachineTest, AboveHighWatermarkEntersSoftPressure) {
    psm.update(0.85, false, kHigh, kLow, kCritical);
    EXPECT_EQ(psm.current_state(), MailboxPressureState::SoftPressure);
}

TEST_F(PressureStateMachineTest, HardFailureEntersHardPressure) {
    psm.update(0.30, true, kHigh, kLow, kCritical);
    EXPECT_EQ(psm.current_state(), MailboxPressureState::HardPressure);
}

TEST_F(PressureStateMachineTest, CriticalWatermarkEntersHardPressure) {
    psm.update(1.05, false, kHigh, kLow, kCritical);
    EXPECT_EQ(psm.current_state(), MailboxPressureState::HardPressure);
}

TEST_F(PressureStateMachineTest, HardPressureHysteresisToRecovering) {
    psm.update(1.05, false, kHigh, kLow, kCritical); // -> HardPressure
    psm.update(0.60, false, kHigh, kLow, kCritical); // still above low
    EXPECT_EQ(psm.current_state(), MailboxPressureState::Recovering);
}

TEST_F(PressureStateMachineTest, RecoveringToNormalBelowLowWatermark) {
    psm.update(0.85, false, kHigh, kLow, kCritical); // -> SoftPressure
    psm.update(0.95, false, kHigh, kLow, kCritical); // -> HardPressure
    psm.update(0.60, false, kHigh, kLow, kCritical); // -> Recovering
    psm.update(0.30, false, kHigh, kLow, kCritical); // below low -> Normal
    EXPECT_EQ(psm.current_state(), MailboxPressureState::Normal);
}

TEST_F(PressureStateMachineTest, SoftPressureStaysAtSoftPressure) {
    psm.update(0.85, false, kHigh, kLow, kCritical); // -> SoftPressure
    psm.update(0.95, false, kHigh, kLow, kCritical); // 0.95 >= 0.80 but < 1.00,
                                                     // stays Soft
    EXPECT_EQ(psm.current_state(), MailboxPressureState::SoftPressure);
}

TEST_F(PressureStateMachineTest, CodeAfterAcceptNormal) {
    EXPECT_EQ(psm.code_after_accept(), EnqueueResultCode::Accepted);
}

TEST_F(PressureStateMachineTest, CodeAfterAcceptUnderPressure) {
    psm.update(0.85, false, kHigh, kLow, kCritical);
    EXPECT_EQ(psm.code_after_accept(), EnqueueResultCode::AcceptedWithSoftPressure);
}

TEST_F(PressureStateMachineTest, SeverityOrdering) {
    EXPECT_EQ(PressureStateMachine::severity(MailboxPressureState::Normal), 0);
    EXPECT_EQ(PressureStateMachine::severity(MailboxPressureState::Recovering), 1);
    EXPECT_EQ(PressureStateMachine::severity(MailboxPressureState::SoftPressure), 2);
    EXPECT_EQ(PressureStateMachine::severity(MailboxPressureState::HardPressure), 3);
}
