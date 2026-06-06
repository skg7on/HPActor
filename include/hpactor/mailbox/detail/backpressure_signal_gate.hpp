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

#include <hpactor/mailbox/detail/pressure_state_machine.hpp>
#include <hpactor/mailbox/mailbox_policy.hpp>

#include <atomic>
#include <cstdint>
#include <optional>

namespace hpactor::mailbox::detail {

/// \brief Rate-limited gate for backpressure signal emission.
///
/// Enforces a minimum interval between backpressure signal emissions per
/// pressure severity level. Escalations (higher severity) bypass the interval
/// to ensure timely notification of worsening conditions. Uses lock-free CAS
/// to coordinate concurrent producer threads.
///
/// \note Thread safety: all methods are lock-free and safe to call from any
///       thread. Internal coordination uses CAS loops on atomic state.
class BackpressureSignalGate {
  public:
    BackpressureSignalGate() = default;

    /// \brief Attempt to acquire an emission slot.
    ///
    /// Returns an emission timestamp if the signal is allowed through; the
    /// gate allows the first signal, signals after the configured interval,
    /// and signals with higher severity than the last emitted (escalation).
    ///
    /// \param[in] now_ns Current monotonic timestamp in nanoseconds.
    /// \param[in] state Current pressure state (determines severity).
    /// \param[in] interval_ms Minimum interval between signals in milliseconds.
    /// \param[in] force If \c true, bypass rate limiting regardless of state.
    /// \return The emission sequence number if acquired, or \c std::nullopt
    ///         if the rate limiter blocked the emission.
    /// \note Thread safety: lock-free CAS — safe to call from any thread.
    std::optional<uint64_t>
    try_acquire(uint64_t now_ns, MailboxPressureState state,
                uint32_t interval_ms, bool force = false) noexcept {
        const uint64_t interval_ns =
            static_cast<uint64_t>(interval_ms) * 1'000'000ULL;
        const auto severity = PressureStateMachine::severity(state);

        uint64_t last = last_signal_ns_.load(std::memory_order_acquire);
        uint8_t last_severity = last_severity_.load(std::memory_order_acquire);

        while (true) {
            const bool first = last == 0;
            const bool interval_elapsed = now_ns >= last + interval_ns;
            const bool escalation = severity > last_severity;

            if (!force && !first && !interval_elapsed && !escalation)
                return std::nullopt;

            if (last_signal_ns_.compare_exchange_weak(last, now_ns,
                                                      std::memory_order_acq_rel,
                                                      std::memory_order_acquire)) {
                last_severity_.store(severity, std::memory_order_release);
                return sequence_.fetch_add(1, std::memory_order_acq_rel) + 1;
            }

            last_severity = last_severity_.load(std::memory_order_acquire);
        }
    }

    /// \brief Current emission sequence number.
    ///
    /// \return The total number of signals emitted since construction.
    /// \note Thread safety: lock-free atomic load — safe to call from any
    ///       thread.
    uint64_t sequence() const noexcept {
        return sequence_.load(std::memory_order_acquire);
    }

  private:
    std::atomic<uint64_t> last_signal_ns_{0};
    std::atomic<uint8_t> last_severity_{0};
    std::atomic<uint64_t> sequence_{0};
};

} // namespace hpactor::mailbox::detail
