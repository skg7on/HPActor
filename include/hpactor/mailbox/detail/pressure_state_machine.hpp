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

#include <hpactor/mailbox/mailbox_policy.hpp>

#include <atomic>
#include <cstdint>

namespace hpactor::mailbox::detail {

/// \brief Hysteresis-based mailbox pressure state machine.
///
/// Tracks pressure state across four levels (Normal, SoftPressure,
/// HardPressure, Recovering) with configurable watermarks to prevent
/// oscillation. Transitions are computed from the current pressure ratio
/// and the configured high/low/critical watermark thresholds.
///
/// \note Thread safety: internal state is an atomic
///       \c MailboxPressureState. \c update() and \c current_state() are
///       safe to call from any thread. The static helper \c severity()
///       and \c next_state() are lock-free.
class PressureStateMachine {
  public:
    PressureStateMachine() = default;

    /// \brief Update the pressure state from the current ratio.
    ///
    /// \param[in] ratio Current pressure ratio (max of count and byte ratios).
    /// \param[in] hard_failure If \c true, immediately transition to
    ///                         \c HardPressure regardless of ratio.
    /// \param[in] high_watermark Ratio threshold for entering SoftPressure.
    /// \param[in] low_watermark Ratio threshold for clearing pressure
    /// (hysteresis).
    /// \param[in] critical_watermark Ratio threshold for entering HardPressure.
    /// \note Thread safety: atomic store with release ordering — safe from
    ///       any thread.
    void update(double ratio, bool hard_failure, double high_watermark,
                double low_watermark, double critical_watermark) noexcept {
        pressure_state_.store(next_state(ratio, hard_failure, high_watermark,
                                         low_watermark, critical_watermark),
                              std::memory_order_release);
    }

    /// \brief Current pressure state.
    ///
    /// \return The most recently stored \c MailboxPressureState.
    /// \note Thread safety: atomic load with acquire ordering — safe from
    ///       any thread.
    MailboxPressureState current_state() const noexcept {
        return pressure_state_.load(std::memory_order_acquire);
    }

    /// \brief Map the current pressure state to an \c EnqueueResultCode.
    ///
    /// Returns \c AcceptedWithSoftPressure when in any non-Normal state,
    /// and \c Accepted otherwise. Used to populate the \c code field after
    /// a successful enqueue.
    ///
    /// \return \c Accepted or \c AcceptedWithSoftPressure.
    /// \note Thread safety: atomic load — safe from any thread.
    EnqueueResultCode code_after_accept() const noexcept {
        auto state = pressure_state_.load(std::memory_order_acquire);
        if (state == MailboxPressureState::SoftPressure ||
            state == MailboxPressureState::HardPressure ||
            state == MailboxPressureState::Recovering) {
            return EnqueueResultCode::AcceptedWithSoftPressure;
        }
        return EnqueueResultCode::Accepted;
    }

    /// \brief Numeric severity of a pressure state (0–3).
    ///
    /// Used by \c BackpressureSignalGate for escalation detection.
    ///
    /// \param[in] state The pressure state to rate.
    /// \return Severity: 0=Normal, 1=Recovering, 2=SoftPressure,
    /// 3=HardPressure.
    /// \note Thread safety: constexpr — safe to call from any context.
    static uint8_t severity(MailboxPressureState state) noexcept {
        switch (state) {
            case MailboxPressureState::Normal:
                return 0;
            case MailboxPressureState::Recovering:
                return 1;
            case MailboxPressureState::SoftPressure:
                return 2;
            case MailboxPressureState::HardPressure:
                return 3;
        }
        return 0;
    }

  private:
    static MailboxPressureState
    next_state(double ratio, bool hard_failure, double high_watermark,
               double low_watermark, double critical_watermark,
               MailboxPressureState current) noexcept {
        if (hard_failure || ratio >= critical_watermark)
            return MailboxPressureState::HardPressure;

        if (current == MailboxPressureState::HardPressure ||
            current == MailboxPressureState::Recovering) {
            if (ratio < low_watermark)
                return MailboxPressureState::Normal;
            return MailboxPressureState::Recovering;
        }

        if (ratio >= high_watermark)
            return MailboxPressureState::SoftPressure;
        if (ratio < low_watermark)
            return MailboxPressureState::Normal;
        return current;
    }

    MailboxPressureState
    next_state(double ratio, bool hard_failure, double high_watermark,
               double low_watermark, double critical_watermark) const noexcept {
        return next_state(ratio, hard_failure, high_watermark, low_watermark,
                          critical_watermark,
                          pressure_state_.load(std::memory_order_acquire));
    }

    std::atomic<MailboxPressureState> pressure_state_{MailboxPressureState::Normal};
};

} // namespace hpactor::mailbox::detail
