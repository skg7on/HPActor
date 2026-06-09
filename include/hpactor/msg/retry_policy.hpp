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
#include <random>

namespace hpactor::msg {

/// \brief Backoff algorithm for retry delay computation.
enum class RetryBackoff : uint8_t {
    Fixed,       ///< Same delay every retry.
    Linear,      ///< Delay scales linearly with attempt number.
    Exponential, ///< Delay doubles each retry attempt.
};

/// \brief Configurable retry policy for at-least-once delivery.
///
/// Controls the maximum number of attempts, per-attempt timeout,
/// backoff algorithm, and optional jitter. When \c max_attempts is
/// 1, no retry is performed (single attempt).
struct RetryPolicy {
    /// Maximum number of delivery attempts (1 = no retry).
    uint8_t max_attempts = 1;

    /// Timeout for each individual send attempt.
    std::chrono::milliseconds per_attempt_timeout{5000};

    /// Initial backoff delay before the first retry.
    std::chrono::milliseconds initial_backoff{100};

    /// Maximum backoff delay (ceiling for exponential growth).
    std::chrono::milliseconds max_backoff{30000};

    /// Backoff algorithm used to compute retry delays.
    RetryBackoff backoff = RetryBackoff::Exponential;

    /// Whether to apply ±25% random jitter to backoff delays.
    bool jitter = true;

    /// \brief Whether retry is enabled (max_attempts > 1).
    [[nodiscard]] bool is_enabled() const noexcept {
        return max_attempts > 1;
    }

    /// \brief Compute the delay before the next retry.
    ///
    /// \param[in] attempt_number 1-based attempt count
    ///                           (1 = first retry).
    /// \return The backoff delay clamped to \c max_backoff,
    ///         with optional ±25% jitter applied.
    [[nodiscard]] std::chrono::milliseconds
    backoff_delay(uint8_t attempt_number) const noexcept {
        using namespace std::chrono;
        int64_t base_ms = 0;
        switch (backoff) {
            case RetryBackoff::Fixed:
                base_ms = initial_backoff.count();
                break;
            case RetryBackoff::Linear:
                base_ms = static_cast<int64_t>(initial_backoff.count()) *
                          static_cast<int64_t>(attempt_number);
                break;
            case RetryBackoff::Exponential: {
                int64_t shift = static_cast<int64_t>(1) << (attempt_number - 1);
                base_ms = initial_backoff.count() * shift;
                break;
            }
        }
        if (base_ms > max_backoff.count()) {
            base_ms = max_backoff.count();
        }
        if (jitter && base_ms > 0) {
            // Thread-local RNG for ±25% jitter.
            static thread_local std::mt19937_64 rng{std::random_device{}()};
            static thread_local std::uniform_int_distribution<int64_t> dist{-25, 25};
            int64_t jitter_pct = dist(rng);
            base_ms = base_ms + (base_ms * jitter_pct / 100);
            if (base_ms < 1)
                base_ms = 1;
        }
        return milliseconds(base_ms);
    }
};

} // namespace hpactor::msg
