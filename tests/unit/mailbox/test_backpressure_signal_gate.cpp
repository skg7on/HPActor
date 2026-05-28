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

#include <hpactor/mailbox/detail/backpressure_signal_gate.hpp>

#include <gtest/gtest.h>

using namespace hpactor::mailbox;
using namespace hpactor::mailbox::detail;

class BackpressureSignalGateTest : public ::testing::Test {
  protected:
    BackpressureSignalGate gate;
    static constexpr uint32_t kIntervalMs = 100;
};

// NOLINTBEGIN(bugprone-unchecked-optional-access)

TEST_F(BackpressureSignalGateTest, FirstSignalAlwaysAcquired) {
    auto seq = gate.try_acquire(1'000'000'000,
                                MailboxPressureState::SoftPressure, kIntervalMs);
    ASSERT_TRUE(seq);
    EXPECT_EQ(*seq, 1u);
}

TEST_F(BackpressureSignalGateTest, SecondSignalWithinIntervalBlocked) {
    gate.try_acquire(1'000'000'000, MailboxPressureState::SoftPressure, kIntervalMs);
    auto seq = gate.try_acquire(1'000'000'050,
                                MailboxPressureState::SoftPressure, kIntervalMs);
    EXPECT_FALSE(seq.has_value());
}

TEST_F(BackpressureSignalGateTest, SignalAfterIntervalAllowed) {
    gate.try_acquire(1'000'000'000, MailboxPressureState::SoftPressure, kIntervalMs);
    uint64_t later =
        1'000'000'000 + static_cast<uint64_t>(kIntervalMs) * 1'000'000ULL;
    auto seq =
        gate.try_acquire(later, MailboxPressureState::SoftPressure, kIntervalMs);
    ASSERT_TRUE(seq);
    EXPECT_EQ(*seq, 2u);
}

TEST_F(BackpressureSignalGateTest, EscalationBypassesInterval) {
    gate.try_acquire(1'000'000'000, MailboxPressureState::SoftPressure, kIntervalMs);
    auto seq = gate.try_acquire(1'000'000'000,
                                MailboxPressureState::HardPressure, kIntervalMs);
    ASSERT_TRUE(seq);
    EXPECT_EQ(*seq, 2u);
}

TEST_F(BackpressureSignalGateTest, DeescalationDoesNotBypassInterval) {
    gate.try_acquire(1'000'000'000, MailboxPressureState::HardPressure, kIntervalMs);
    auto seq = gate.try_acquire(1'000'000'001,
                                MailboxPressureState::SoftPressure, kIntervalMs);
    EXPECT_FALSE(seq.has_value());
}

TEST_F(BackpressureSignalGateTest, SequenceMonotonicallyIncreases) {
    auto s1 = gate.try_acquire(1'000'000'000, MailboxPressureState::Normal,
                               kIntervalMs);
    uint64_t later =
        1'000'000'000 + static_cast<uint64_t>(kIntervalMs) * 1'000'000ULL;
    auto s2 = gate.try_acquire(later, MailboxPressureState::Normal, kIntervalMs);
    ASSERT_TRUE(s1);
    ASSERT_TRUE(s2);
    EXPECT_LT(*s1, *s2);
}

TEST_F(BackpressureSignalGateTest, ZeroIntervalAllowsEveryCall) {
    auto s1 = gate.try_acquire(1'000'000'000, MailboxPressureState::Normal, 0);
    auto s2 = gate.try_acquire(1'000'000'001, MailboxPressureState::Normal, 0);
    ASSERT_TRUE(s1);
    ASSERT_TRUE(s2);
}

// NOLINTEND(bugprone-unchecked-optional-access)
