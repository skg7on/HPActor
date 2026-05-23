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

#include <chrono>
#include <cstdint>

namespace hpactor {

/// \brief Per-actor quarantine and circuit breaker policy.
///
/// All thresholds default to 0 (disabled). Circuit breaker tracking,
/// failure-rate windows, and quarantine escalation are only active
/// when \c enabled is \c true. TOML-configurable per-actor or via
/// system-level defaults in \c [system.quarantine].
///
/// \note Thread safety: immutable after actor construction. Reads
///       from the scheduler thread require no synchronization.
struct QuarantinePolicy {
    /// Master switch — when \c false, no quarantine or circuit breaker
    /// overhead is incurred.
    bool enabled = false;

    /// When \c true and \c enabled, exceeding \c max_restarts within
    /// the supervision window quarantines the child instead of stopping it.
    bool escalate_on_max_restarts = true;

    /// Failures/sec threshold for circuit breaker trip. 0 disables
    /// failure-rate tripping.
    uint32_t failure_rate_threshold = 0;

    /// Timeouts/sec threshold for circuit breaker trip. 0 disables
    /// timeout-rate tripping.
    uint32_t timeout_rate_threshold = 0;

    /// Mailbox pressure ratio (0.0–1.0) for sustained overload detection.
    /// 0.0 disables pressure-based tripping.
    float mailbox_pressure_threshold = 0.0f;

    /// Duration the circuit breaker remains \c kOpen before transitioning
    /// to \c kHalfOpen.
    std::chrono::milliseconds cooldown_period{30'000};

    /// Sliding window over which failure and timeout rates are computed.
    std::chrono::milliseconds observation_window{10'000};

    /// Number of consecutive circuit trips before escalating to quarantine.
    /// 0 disables circuit-to-quarantine escalation.
    uint32_t max_circuit_trips = 3;
};

} // namespace hpactor
