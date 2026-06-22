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

namespace hpactor::actor::durable {

/// \brief Strategy for handling recovery failures during durable actor
///        reactivation.
///
/// Determines how the runtime responds when event replay or snapshot
/// restoration fails. Selected per-actor via
/// \c PassivationConfig::recovery_policy.
enum class RecoveryPolicy : uint8_t {
    /// Stop the actor immediately. Supervision policy decides whether to
    /// restart.
    FailActor = 0,
    /// Move the actor into quarantine — messages are rejected but the actor
    /// can be inspected for diagnostics.
    QuarantineActor = 1,
    /// Skip the corrupt event and continue replay. The actor is responsible
    /// for detecting gaps or inconsistencies in its state.
    SkipCorruptEvent = 2,
};

/// \brief Human-readable snake_case string for logging and CLI.
///
/// \param[in] policy The recovery policy to stringify.
/// \return A null-terminated snake_case string literal (e.g. "fail_actor",
///         "quarantine_actor"). Never returns nullptr.
constexpr const char* to_string(RecoveryPolicy policy) noexcept {
    switch (policy) {
        case RecoveryPolicy::FailActor:
            return "fail_actor";
        case RecoveryPolicy::QuarantineActor:
            return "quarantine_actor";
        case RecoveryPolicy::SkipCorruptEvent:
            return "skip_corrupt_event";
    }
    return "unknown";
}

} // namespace hpactor::actor::durable
