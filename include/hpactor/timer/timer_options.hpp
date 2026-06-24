// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <chrono>
#include <cstdint>

namespace hpactor::sched {

/// Options for timer scheduling — passed to schedule_after() and
/// ActorContext::schedule() to control priority, deadline, trace
/// propagation, and coalescing tolerance.
struct TimerOptions {
    /// Priority level (0 = highest, 255 = lowest).
    uint8_t priority{0};

    /// Absolute deadline override.  Zero means "use expire time."
    std::chrono::milliseconds deadline{0};

    /// Propagate the current trace context onto timer-fired messages.
    bool propagate_trace{true};

    /// Coalescing tolerance in nanoseconds.  Zero means no coalescing.
    uint64_t tolerance_ns{0};

    static TimerOptions defaults() {
        return TimerOptions{};
    }

    static TimerOptions high_priority() {
        TimerOptions opts;
        opts.priority = 0;
        return opts;
    }
};

} // namespace hpactor::sched
