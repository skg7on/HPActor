// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#include <hpactor/sched/worker_park.hpp>

#ifdef __linux__
#    include <algorithm>
#    include <linux/futex.h>
#    include <sys/syscall.h>
#    include <unistd.h>

static void futex_wake_one(std::atomic<uint32_t>* addr) noexcept {
    ::syscall(SYS_futex, reinterpret_cast<uint32_t*>(addr), FUTEX_WAKE_PRIVATE,
              1, nullptr, nullptr, 0);
}

static void futex_wait(std::atomic<uint32_t>* addr, uint32_t expected,
                       int64_t timeout_ns) noexcept {
    struct timespec ts{timeout_ns / 1'000'000'000LL, timeout_ns % 1'000'000'000LL};
    ::syscall(SYS_futex, reinterpret_cast<uint32_t*>(addr), FUTEX_WAIT_PRIVATE,
              expected, &ts, nullptr, 0);
}

namespace hpactor::sched {

void WorkerPark::wake() noexcept {
    seq.fetch_add(1, std::memory_order_acq_rel);
    if (parked.load(std::memory_order_acquire)) {
        futex_wake_one(&seq);
    }
}

uint32_t WorkerPark::begin_park() noexcept {
    // Read snap BEFORE storing parked.  If a producer increments seq
    // between snap and parked=true, the recheck finds the work.  If
    // the producer runs between parked=true and wait(), seq != snap
    // and wait() returns immediately.
    uint32_t snap = seq.load(std::memory_order_acquire);
    parked.store(true, std::memory_order_seq_cst);
    return snap;
}

void WorkerPark::wait(uint32_t expected, int64_t timeout_ns) noexcept {
    futex_wait(&seq, expected, timeout_ns);
}

void WorkerPark::end_park() noexcept {
    parked.store(false, std::memory_order_release);
}

} // namespace hpactor::sched
#endif // __linux__
