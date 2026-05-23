// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#include <hpactor/actor/quarantine_reason.hpp>

namespace hpactor {

const char* to_string(QuarantineReason reason) noexcept {
    switch (reason) {
        case QuarantineReason::SupervisionEscalation:
            return "supervision_escalation";
        case QuarantineReason::CircuitBreakerTrip:
            return "circuit_breaker_trip";
        case QuarantineReason::MailboxPressure:
            return "mailbox_pressure";
        case QuarantineReason::OperatorAction:
            return "operator_action";
        case QuarantineReason::RecoveryFailure:
            return "recovery_failure";
    }
    return "unknown";
}

} // namespace hpactor
