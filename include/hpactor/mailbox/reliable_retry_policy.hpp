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

namespace hpactor::mailbox {

/// \brief Duration type alias for retry backoff calculations.
using Duration = std::chrono::milliseconds;

/// \brief Configurable retry policy with exponential backoff.
///
/// Controls the number of retries and delay between each attempt.
/// Backoff doubles each attempt (configurable via backoff_multiplier)
/// and is capped at max_backoff.
struct ReliableRetryPolicy {
    uint32_t max_retries = 3;
    std::chrono::milliseconds initial_backoff{100};
    std::chrono::milliseconds max_backoff{10000};
    double backoff_multiplier = 2.0;

    /// \brief Compute the backoff delay for the given attempt number.
    ///
    /// \param attempt The retry attempt number (1-based).
    /// \return The backoff Duration, capped at max_backoff.
    Duration backoff_for_attempt(uint32_t attempt) const {
        if (attempt == 0)
            return Duration::zero();
        double ms = static_cast<double>(initial_backoff.count());
        for (uint32_t i = 1; i < attempt; ++i) {
            ms *= backoff_multiplier;
        }
        auto dur = std::chrono::milliseconds(static_cast<int64_t>(ms));
        return dur > max_backoff ? max_backoff : dur;
    }

    /// \brief Check whether a retry should be attempted.
    ///
    /// \param attempt The number of retries already attempted.
    /// \return true if attempt < max_retries.
    bool should_retry(uint32_t attempt) const {
        return attempt < max_retries;
    }
};

} // namespace hpactor::mailbox
