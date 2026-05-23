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

/// \brief Circuit breaker state machine for per-actor failure protection.
///
/// \c kClosed is normal operation. \c kOpen rejects all user messages
/// until the cooldown expires. \c kHalfOpen admits a single probe
/// message to test recovery.
///
/// \note Thread safety: single-writer — only the owning actor's
///       scheduler thread mutates the state.
enum class CircuitBreakerState : uint8_t {
    kClosed = 0,   ///< Normal operation — messages flow through.
    kOpen = 1,     ///< All user messages rejected with \c CircuitOpen.
    kHalfOpen = 2, ///< Single probe message allowed through.
};

/// \brief Human-readable string for a circuit breaker state.
///
/// \param[in] state The circuit breaker state.
/// \return \c "closed", \c "open", or \c "half_open". Never \c nullptr.
constexpr const char* to_string(CircuitBreakerState state) noexcept {
    switch (state) {
        case CircuitBreakerState::kClosed:
            return "closed";
        case CircuitBreakerState::kOpen:
            return "open";
        case CircuitBreakerState::kHalfOpen:
            return "half_open";
    }
    return "unknown";
}

/// \brief Per-actor circuit breaker tracker.
///
/// Owned by the actor and mutated only from the scheduler thread.
/// Tracks state transitions, trip count, cooldown timing, and an
/// exponential moving average of the failure rate.
///
/// \note Thread safety: single-writer (scheduler thread). No locking
///       required for read or write.
struct CircuitBreakerTracker {
    /// Current circuit breaker state.
    CircuitBreakerState state{CircuitBreakerState::kClosed};
    /// Number of times the circuit has tripped (Open) in the current
    /// episode.
    uint32_t trip_count{0};
    /// \c true when a half-open probe message is in flight. Prevents
    /// more than one probe at a time.
    bool half_open_probe_in_flight{false};
    /// Monotonic timestamp when the circuit last transitioned to
    /// \c kOpen. Used for cooldown expiry.
    std::chrono::steady_clock::time_point opened_at{};
    /// Exponential moving average of the failure rate (failures/sec).
    double failure_ema{0.0};
};

} // namespace hpactor
