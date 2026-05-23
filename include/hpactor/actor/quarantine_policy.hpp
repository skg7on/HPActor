// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <chrono>
#include <cstdint>

namespace hpactor {

/// Per-actor quarantine and circuit breaker policy.
///
/// All thresholds default to 0 (disabled). Circuit breaker tracking,
/// failure-rate windows, and quarantine escalation are only allocated
/// when \c enabled is true.
struct QuarantinePolicy {
    /// Master switch — when false, no quarantine/circuit-breaker overhead.
    bool enabled = false;

    /// When true, exceeding max_restarts quarantines instead of stopping.
    bool escalate_on_max_restarts = true;

    /// Failures/sec threshold for circuit breaker. 0 = disabled.
    uint32_t failure_rate_threshold = 0;

    /// Timeouts/sec threshold for circuit breaker. 0 = disabled.
    uint32_t timeout_rate_threshold = 0;

    /// Mailbox pressure ratio (0.0–1.0) for sustained overload detection.
    /// 0.0 = disabled.
    float mailbox_pressure_threshold = 0.0f;

    /// How long the circuit breaker stays open before transitioning to
    /// half-open.
    std::chrono::milliseconds cooldown_period{30'000};

    /// Sliding window over which failure/timeout rates are computed.
    std::chrono::milliseconds observation_window{10'000};

    /// Number of consecutive circuit trips before escalating to quarantine.
    /// 0 = never escalate from circuit breaker alone.
    uint32_t max_circuit_trips = 3;
};

} // namespace hpactor
