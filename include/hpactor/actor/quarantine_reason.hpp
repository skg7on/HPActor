// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>

namespace hpactor {

/// Why an actor was placed into quarantine. Distinct from FailureReason —
/// this classifies the trigger, not the delivery outcome.
enum class QuarantineReason : uint8_t {
    SupervisionEscalation = 0, ///< max_restarts exceeded in observation window
    CircuitBreakerTrip = 1,    ///< failure/timeout rate threshold exceeded
    MailboxPressure = 2,       ///< sustained mailbox overload
    OperatorAction = 3,        ///< explicit CLI / admin quarantine
    RecoveryFailure = 4,       ///< recovery attempt failed
};

/// \brief Human-readable snake_case string for each quarantine reason.
///
/// \param[in] reason The quarantine reason.
/// \return A null-terminated string literal (e.g. "supervision_escalation").
///         Never returns nullptr.
const char* to_string(QuarantineReason reason) noexcept;

} // namespace hpactor
