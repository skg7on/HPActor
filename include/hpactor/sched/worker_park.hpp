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

#include <atomic>
#include <cstdint>

namespace hpactor::sched {

/// \brief Lightweight worker park/unpark primitive.
///
/// Replaces `sleep_mutex_` + `sleep_cv_` + `is_blocking_` in WorkerState with
/// a futex-based EventCount that eliminates the mutex acquisition on the wakeup
/// hot path.
///
/// Protocol:
///   Producer  — wake(): seq.fetch_add + conditional futex_wake (no mutex).
///   Worker    — begin_park() → check work → wait(snap, timeout_ns) →
///   end_park().
///
/// Lost-wakeup safety: begin_park() stores parked=true with seq_cst ordering
/// before the worker's condition recheck.  If a producer increments seq between
/// begin_park() and wait(), wait() sees seq != snap and returns immediately.
/// If the producer ran before begin_park() the recheck finds the work.
///
/// Platform: futex on Linux, __ulock on macOS, condition_variable fallback.
struct alignas(64) WorkerPark {
    /// Monotonically increasing sequence; futex address on Linux/macOS.
    std::atomic<uint32_t> seq{0};
    /// True while the worker is parked (seq_cst store in begin_park).
    std::atomic<bool> parked{false};

    /// \brief Signal the parked worker (producer side, lock-free).
    ///
    /// Increments seq then issues a platform wake if the worker is parked.
    /// Safe to call from any thread; idempotent if not parked.
    void wake() noexcept;

    /// \brief Advertise parking intent and return the current sequence.
    ///
    /// Must be followed by a condition recheck, then wait() if still no work.
    /// \return Snapshot of seq to pass to wait().
    uint32_t begin_park() noexcept;

    /// \brief Sleep until seq changes or timeout elapses.
    ///
    /// \param[in] expected Sequence value captured by begin_park().
    /// \param[in] timeout_ns Maximum sleep duration in nanoseconds.
    void wait(uint32_t expected, int64_t timeout_ns) noexcept;

    /// \brief Clear the parked flag after waking or finding work.
    void end_park() noexcept;
};

} // namespace hpactor::sched
