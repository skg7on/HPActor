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
