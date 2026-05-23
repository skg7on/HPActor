// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>
#include <hpactor/actor/quarantine_reason.hpp>

#include <string>

using namespace hpactor;

TEST(QuarantineReasonTest, AllValuesDefined) {
    auto reasons = {
        QuarantineReason::SupervisionEscalation,
        QuarantineReason::CircuitBreakerTrip,
        QuarantineReason::MailboxPressure,
        QuarantineReason::OperatorAction,
        QuarantineReason::RecoveryFailure,
    };
    for (auto r : reasons) {
        EXPECT_NE(std::string(to_string(r)), "unknown");
    }
}

TEST(QuarantineReasonTest, SupervisionEscalationToString) {
    EXPECT_EQ(std::string(to_string(QuarantineReason::SupervisionEscalation)),
              "supervision_escalation");
}

TEST(QuarantineReasonTest, CircuitBreakerTripToString) {
    EXPECT_EQ(std::string(to_string(QuarantineReason::CircuitBreakerTrip)), "ci"
                                                                            "rc"
                                                                            "ui"
                                                                            "t_"
                                                                            "br"
                                                                            "ea"
                                                                            "ke"
                                                                            "r_"
                                                                            "tr"
                                                                            "i"
                                                                            "p");
}

TEST(QuarantineReasonTest, MailboxPressureToString) {
    EXPECT_EQ(std::string(to_string(QuarantineReason::MailboxPressure)), "mailb"
                                                                         "ox_"
                                                                         "press"
                                                                         "ure");
}

TEST(QuarantineReasonTest, OperatorActionToString) {
    EXPECT_EQ(std::string(to_string(QuarantineReason::OperatorAction)), "operat"
                                                                        "or_"
                                                                        "actio"
                                                                        "n");
}

TEST(QuarantineReasonTest, RecoveryFailureToString) {
    EXPECT_EQ(std::string(to_string(QuarantineReason::RecoveryFailure)), "recov"
                                                                         "ery_"
                                                                         "failu"
                                                                         "re");
}

TEST(QuarantineReasonTest, DistinctValues) {
    // All reasons must have different integer values.
    EXPECT_NE(static_cast<uint8_t>(QuarantineReason::SupervisionEscalation),
              static_cast<uint8_t>(QuarantineReason::CircuitBreakerTrip));
    EXPECT_NE(static_cast<uint8_t>(QuarantineReason::CircuitBreakerTrip),
              static_cast<uint8_t>(QuarantineReason::MailboxPressure));
    EXPECT_NE(static_cast<uint8_t>(QuarantineReason::MailboxPressure),
              static_cast<uint8_t>(QuarantineReason::OperatorAction));
    EXPECT_NE(static_cast<uint8_t>(QuarantineReason::OperatorAction),
              static_cast<uint8_t>(QuarantineReason::RecoveryFailure));
}
