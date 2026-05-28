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

class BackpressureSignalGate {
  public:
    BackpressureSignalGate() = default;

    std::optional<uint64_t> try_acquire(uint64_t now_ns, MailboxPressureState state,
                                        uint32_t interval_ms) noexcept {
        const uint64_t interval_ns =
            static_cast<uint64_t>(interval_ms) * 1'000'000ULL;
        const auto severity = PressureStateMachine::severity(state);

        uint64_t last = last_signal_ns_.load(std::memory_order_acquire);
        uint8_t last_severity = last_severity_.load(std::memory_order_acquire);

        while (true) {
            const bool first = last == 0;
            const bool interval_elapsed = now_ns >= last + interval_ns;
            const bool escalation = severity > last_severity;

            if (!first && !interval_elapsed && !escalation)
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

    uint64_t sequence() const noexcept {
        return sequence_.load(std::memory_order_acquire);
    }

  private:
    std::atomic<uint64_t> last_signal_ns_{0};
    std::atomic<uint8_t> last_severity_{0};
    std::atomic<uint64_t> sequence_{0};
};

} // namespace hpactor::mailbox::detail
