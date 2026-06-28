// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <atomic>
#include <cstdint>

namespace hpactor::sched {

/// \brief Linux-only futex-based worker park primitive.
///
/// On Linux, replaces is_blocking_ + sleep_mutex_ + sleep_cv_ in the wakeup
/// hot path.  On other platforms the methods are no-ops and the existing CV
/// implementation in WorkerState is used instead.
///
/// Protocol (lost-wakeup safe):
///   Producer — wake(): seq.fetch_add + conditional futex_wake (no mutex).
///   Worker   — begin_park() → recheck work → wait(snap, timeout_ns) →
///   end_park().
///
/// begin_park() reads seq BEFORE storing parked=true so that an interleaving
/// producer is detected by the worker's recheck or the seq mismatch in wait().
struct alignas(64) WorkerPark {
    std::atomic<uint32_t> seq{0};
    std::atomic<bool> parked{false};

#ifdef __linux__
    /// \brief Signal the parked worker (producer side, lock-free).
    void wake() noexcept;

    /// \brief Advertise parking intent; return current seq for wait().
    uint32_t begin_park() noexcept;

    /// \brief Sleep until seq changes or timeout elapses (nanoseconds).
    void wait(uint32_t expected, int64_t timeout_ns) noexcept;

    /// \brief Clear parked flag.
    void end_park() noexcept;

    /// \brief True when the worker is parked (used for diag).
    bool is_parked() const noexcept {
        return parked.load(std::memory_order_acquire);
    }
#else
    // No-ops — existing CV path is used.
    void wake() noexcept {}
    uint32_t begin_park() noexcept {
        return 0;
    }
    void wait(uint32_t, int64_t) noexcept {}
    void end_park() noexcept {}
    bool is_parked() const noexcept {
        return false;
    }
#endif
};

} // namespace hpactor::sched
