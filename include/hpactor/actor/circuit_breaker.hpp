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

/// Circuit breaker state machine — per-actor, single-writer (scheduler
/// thread).
enum class CircuitBreakerState : uint8_t {
    kClosed = 0,   ///< Normal operation — messages flow through.
    kOpen = 1,     ///< All user messages rejected with CircuitOpen.
    kHalfOpen = 2, ///< Single probe message allowed through.
};

/// \brief Human-readable string for the circuit breaker state.
///
/// \param[in] state The circuit breaker state.
/// \return "closed", "open", or "half_open".
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

/// Per-actor circuit breaker tracker.
///
/// Owned by the actor, mutated only from the scheduler thread. Tracks
/// state, trip count, cooldown expiry, and EMA failure rate.
struct CircuitBreakerTracker {
    CircuitBreakerState state{CircuitBreakerState::kClosed};
    uint32_t trip_count{0};
    bool half_open_probe_in_flight{false};
    std::chrono::steady_clock::time_point opened_at{};
    double failure_ema{0.0};
};

} // namespace hpactor
