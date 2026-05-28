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

class PressureStateMachine {
  public:
    PressureStateMachine() = default;

    void update(double ratio, bool hard_failure, double high_watermark,
                double low_watermark, double critical_watermark) noexcept {
        pressure_state_.store(next_state(ratio, hard_failure, high_watermark,
                                         low_watermark, critical_watermark),
                              std::memory_order_release);
    }

    MailboxPressureState current_state() const noexcept {
        return pressure_state_.load(std::memory_order_acquire);
    }

    EnqueueResultCode code_after_accept() const noexcept {
        auto state = pressure_state_.load(std::memory_order_acquire);
        if (state == MailboxPressureState::SoftPressure ||
            state == MailboxPressureState::HardPressure ||
            state == MailboxPressureState::Recovering) {
            return EnqueueResultCode::AcceptedWithSoftPressure;
        }
        return EnqueueResultCode::Accepted;
    }

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
