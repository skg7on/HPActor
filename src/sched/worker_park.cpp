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

#include <hpactor/sched/worker_park.hpp>

#include <algorithm>

#if defined(__linux__)
#    include <linux/futex.h>
#    include <sys/syscall.h>
#    include <unistd.h>
static void futex_wake_one(std::atomic<uint32_t>* addr) noexcept {
    ::syscall(SYS_futex, reinterpret_cast<uint32_t*>(addr), FUTEX_WAKE_PRIVATE,
              1, nullptr, nullptr, 0);
}
static void futex_wait_val(std::atomic<uint32_t>* addr, uint32_t expected,
                           int64_t timeout_ns) noexcept {
    struct timespec ts{timeout_ns / 1'000'000'000LL, timeout_ns % 1'000'000'000LL};
    ::syscall(SYS_futex, reinterpret_cast<uint32_t*>(addr), FUTEX_WAIT_PRIVATE,
              expected, &ts, nullptr, 0);
}

#elif defined(__APPLE__)
// Private macOS API — stable since 10.12, used by libc++/GCD.
// No public header; symbols are declared directly.
extern "C" int
__ulock_wait(uint32_t op, void* addr, uint64_t val, uint32_t timeout_us);
extern "C" int __ulock_wake(uint32_t op, void* addr, uint64_t wake_val);
#    define UL_COMPARE_AND_WAIT 1

static void futex_wake_one(std::atomic<uint32_t>* addr) noexcept {
    __ulock_wake(UL_COMPARE_AND_WAIT, addr, 0);
}
static void futex_wait_val(std::atomic<uint32_t>* addr, uint32_t expected,
                           int64_t timeout_ns) noexcept {
    // __ulock_wait timeout is in microseconds; 0 = indefinite
    uint32_t timeout_us =
        timeout_ns > 0
            ? static_cast<uint32_t>(std::max(int64_t{1}, timeout_ns / 1'000LL))
            : 0u;
    __ulock_wait(UL_COMPARE_AND_WAIT, addr, expected, timeout_us);
}

#else
// Fallback: spin briefly then yield. Not as efficient but portable.
#    include <thread>
static void futex_wake_one(std::atomic<uint32_t>*) noexcept {}
static void futex_wait_val(std::atomic<uint32_t>* addr, uint32_t expected,
                           int64_t timeout_ns) noexcept {
    auto deadline =
        std::chrono::steady_clock::now() + std::chrono::nanoseconds(timeout_ns);
    while (addr->load(std::memory_order_relaxed) == expected &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
}
#endif

namespace hpactor::sched {

void WorkerPark::wake() noexcept {
    seq.fetch_add(1, std::memory_order_acq_rel);
    if (parked.load(std::memory_order_acquire)) {
        futex_wake_one(&seq);
    }
}

uint32_t WorkerPark::begin_park() noexcept {
    // seq_cst so the producer's parked.load sees this store before its seq
    // fetch_add completes (prevents lost wakeup).
    parked.store(true, std::memory_order_seq_cst);
    return seq.load(std::memory_order_acquire);
}

void WorkerPark::wait(uint32_t expected, int64_t timeout_ns) noexcept {
    // Returns immediately if seq != expected (producer already woke us).
    futex_wait_val(&seq, expected, timeout_ns);
}

void WorkerPark::end_park() noexcept {
    parked.store(false, std::memory_order_release);
}

} // namespace hpactor::sched
