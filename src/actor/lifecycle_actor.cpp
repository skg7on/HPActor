// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#include <hpactor/actor/lifecycle_actor.hpp>
#include <hpactor/types/types.hpp>

namespace hpactor {

void LifecycleActor::on_fail(error /*err*/) {
    // Default no-op; subclasses override for failure-specific cleanup
}

bool LifecycleActor::transition(LifecycleState to) {
    LifecycleState from = state();

    // Validate that `to` is in this state's transition list
    const auto& def = kStateMachine[static_cast<int>(from)];
    bool legal = false;
    for (uint8_t i = 0; i < def.num_transitions; ++i) {
        if (def.transitions[i] == to) {
            legal = true;
            break;
        }
    }
    if (!legal)
        return false;

    // CAS: only change if still in `from`
    uint8_t expected = static_cast<uint8_t>(from);
    uint8_t desired = static_cast<uint8_t>(to);
    if (!state_.compare_exchange_strong(expected, desired, std::memory_order_acq_rel,
                                        std::memory_order_acquire)) {
        return false;
    }

    // Post-transition: invoke the hook for this transition.
    // on_fail() uses the stored failure_reason_ (set by caller before
    // transition).
    if (to == LifecycleState::kActive && (from == LifecycleState::kStarting ||
                                          from == LifecycleState::kRecovering)) {
        on_start();
    } else if (to == LifecycleState::kDraining) {
        on_drain();
    } else if (to == LifecycleState::kStopping) {
        on_stop();
    } else if (to == LifecycleState::kStopped) {
        on_deactivate();
    } else if (to == LifecycleState::kFailed) {
        on_fail(failure_reason_);
    } else if (to == LifecycleState::kStarting &&
               (from == LifecycleState::kFailed || from == LifecycleState::kStopped)) {
        on_restart();
    } else if (to == LifecycleState::kRecovering) {
        on_recover();
    }

    return true;
}

} // namespace hpactor
