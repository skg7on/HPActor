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

#include <hpactor/fault/fault_controller.hpp>
#include <hpactor/mem/memory_config.hpp>
#include <hpactor/mem/thread_local_allocator.hpp>
#include <hpactor/sched/scheduler.hpp>
#include <hpactor/sched/worker_thread.hpp>

#include <chrono>
#include <mutex>
#include <thread>

#ifdef __linux__
#    include <sys/syscall.h>
#    include <unistd.h>
#endif

namespace hpactor::sched {

// Static member definitions for adaptive backoff calibration.
//
// `shared_calibration_` uses default-constructed BackoffCalibration until
// the first worker runs the calibration probe.  `calibration_once_`
// ensures the probe runs exactly once.  `test_calibration_override_`
// defaults to nullptr (no override).
BackoffCalibration WorkerThread::shared_calibration_;
std::once_flag WorkerThread::calibration_once_;
const BackoffCalibration* WorkerThread::test_calibration_override_ = nullptr;

namespace {

BackoffCalibration run_calibration_probe() {
    BackoffCalibration cal;

    // 1. Yield effectiveness: time 1000 consecutive sched_yield() calls.
    //    If yield actually deschedules, this takes microseconds per call.
    //    If yield is a no-op (Linux CFS), this is near-CPU-spin speed.
    {
        auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 1000; ++i) {
            std::this_thread::yield();
        }
        auto t1 = std::chrono::steady_clock::now();
        auto elapsed_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
        // Pure spin: ~10-50 ns/iter -> ~10-50 us total for 1000 iters.
        // Real yield: ~1-10 us/iter -> ~1-10 ms total for 1000 iters.
        // Threshold at 500 us: below is spin, above is real yield.
        cal.yield_is_effective = (elapsed_ns > 500'000);
    }

    // 2. Timer granularity: sleep at increasing durations and measure
    //    actual elapsed time.  Find the first duration the kernel honours.
    {
        constexpr uint32_t kDurationsNs[] = {1'000,   10'000,  50'000,
                                             100'000, 500'000, 1'000'000};
        uint32_t best = 50'000; // fallback
        for (uint32_t dur_ns : kDurationsNs) {
            auto t0 = std::chrono::steady_clock::now();
            std::this_thread::sleep_for(std::chrono::nanoseconds(dur_ns));
            auto t1 = std::chrono::steady_clock::now();
            auto actual_ns =
                std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
            // If the kernel honoured this sleep within a factor of 4,
            // this duration is "effective."
            if (actual_ns > 0 && actual_ns <= static_cast<int64_t>(dur_ns) * 4) {
                best = dur_ns;
                break; // first effective duration is the granularity floor
            }
        }
        cal.min_effective_sleep_ns = best;
    }

    // 3. Derived thresholds.
    cal.spin_threshold_ns = cal.yield_is_effective ? 20'000 : 0;
    // Polling budget: platform-adaptive floor.  On macOS where yield
    // actually deschedules, a short burst of yields is sufficient for
    // burst absorption before CV entry.  On Linux where yield is a
    // no-op, keep a longer budget to absorb bursts via nanosleep.
    {
        uint32_t floor_ns = cal.yield_is_effective ? 200'000u : 1'000'000u;
        cal.polling_budget_ns = std::max(floor_ns, cal.min_effective_sleep_ns * 20);
    }

    return cal;
}

} // anonymous namespace

// Thread-local pointer to the current worker's frame pool
thread_local CoroutineFramePool* tl_frame_pool = nullptr;

// Thread-local worker ID (declared in scheduler.cpp, used by placement layer)
extern thread_local uint32_t tl_current_worker_id;

// ── Platform-specific backoff constants (removed; time-based backoff now
//    uses BackoffCalibration values measured at startup.) ──────────────

WorkerThread::WorkerThread(const Config& config)
    : config_(config), local_queue_(config.priority_levels) {
    if (config_.enable_thread_allocator) {
        allocator_ = new mem::ThreadLocalAllocator();
    }
}

WorkerThread::~WorkerThread() {
    stop();
    delete allocator_;
}

void WorkerThread::start() {
    if (running_.load(std::memory_order_acquire)) {
        return;
    }
    running_.store(true, std::memory_order_release);
    stop_requested_.store(false, std::memory_order_release);

    // Populate calibration: use test override if set, otherwise run probe once.
    if (test_calibration_override_) {
        calibration_ = *test_calibration_override_;
    } else {
        std::call_once(calibration_once_,
                       [] { shared_calibration_ = run_calibration_probe(); });
        calibration_ = shared_calibration_;
    }

    thread_ = std::thread([this] {
#ifdef __linux__
        native_tid_.store(static_cast<uint64_t>(syscall(SYS_gettid)),
                          std::memory_order_relaxed);
#endif
        if (frame_pool_) {
            tl_frame_pool = frame_pool_;
        }
        if (allocator_) {
            mem::set_thread_allocator(allocator_);
        }
        if (fault_controller_) {
            reinterpret_cast<::hpactor::fault::FaultController*>(fault_controller_)
                ->install();
        }
        thread_loop();
        if (fault_controller_) {
            reinterpret_cast<::hpactor::fault::FaultController*>(fault_controller_)
                ->remove();
        }
    });
}

void WorkerThread::stop() {
    stop_requested_.store(true, std::memory_order_release);
    running_.store(false, std::memory_order_release);
    // Wake the worker if it is blocked on its sleep CV so it can observe
    // the stop_requested_ flag and exit the thread loop promptly.
    if (owner_ && config_.worker_index < owner_->placement_workers().size()) {
        owner_->placement_workers()[config_.worker_index].wake_if_blocking();
    }
    if (thread_.joinable()) {
        thread_.join();
    }
}

void WorkerThread::push(uint8_t priority, WorkItem item) {
    local_queue_.push(priority, item);
}

bool WorkerThread::pop(WorkItem& out) {
    return local_queue_.pop(out);
}

bool WorkerThread::steal(WorkItem& out) {
    // Steal from the highest priority queue first
    for (uint32_t i = 0; i < local_queue_.num_levels(); ++i) {
        if (local_queue_.steal(out)) {
            return true;
        }
    }
    return false;
}

size_t WorkerThread::depth() const {
    return local_queue_.depth_approx();
}

CoroutineFramePool::Frame* WorkerThread::acquire_frame() {
    if (frame_pool_) {
        return frame_pool_->acquire();
    }
    return nullptr;
}

void WorkerThread::release_frame(CoroutineFramePool::Frame* frame) {
    if (frame_pool_ && frame) {
        frame_pool_->release(frame);
    }
}

bool WorkerThread::try_steal(WorkItem& out) {
    if (owner_) {
        return owner_->try_steal(out);
    }
    return false;
}

void WorkerThread::process_work_item(const WorkItem& item) {
    diag_work_found_.fetch_add(1, std::memory_order_relaxed);
    // Reset idle tracking — worker is active.
    idle_since_ = std::chrono::steady_clock::time_point{};
    consecutive_empty_wakes_.store(0, std::memory_order_relaxed);
    in_cv_model_.store(false, std::memory_order_relaxed);
    if (processor_) {
        processor_(item);
    }
}

bool WorkerThread::try_find_and_process_work() {
    WorkItem item;

    // Phase 1 (fast path): only check local queues.  Stealing is deferred
    // to the CV double-check in enter_cv_block() to avoid expensive
    // cross-core atomic scans on every backoff iteration during polling.
    if (owner_) {
        if (owner_->pop_local(item, config_.worker_index)) {
            process_work_item(item);
            return true;
        }
    } else {
        if (pop(item)) {
            process_work_item(item);
            return true;
        }
    }

    return false;
}

bool WorkerThread::try_poll_idle() {
    auto now = std::chrono::steady_clock::now();

    // Record idle start on first idle iteration.
    if (idle_since_ == std::chrono::steady_clock::time_point{}) {
        idle_since_ = now;
    }

    auto elapsed_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(now - idle_since_).count();

    if (static_cast<uint64_t>(elapsed_ns) < calibration_.polling_budget_ns) {
        diag_idle_iters_.fetch_add(1, std::memory_order_relaxed);
        increment_donations();

        // Periodic steal attempt during polling (every 64 iterations).
        // Stealing on every backoff iteration burns CPU with cross-core
        // atomic traffic (dominant on Linux x86_64); never stealing forces
        // a full CV entry cycle just to discover cross-worker work (adds
        // ~600us CV-entry overhead on macOS).  Every 64th balances both.
        if ((diag_idle_iters_.load(std::memory_order_relaxed) & 0x3F) == 0) {
            WorkItem stolen;
            if (try_steal(stolen)) {
                process_work_item(stolen);
                return true; // work found — exit polling, reset via
                             // process_work_item
            }
        }

        backoff(std::chrono::nanoseconds(elapsed_ns));
        return true;
    }
    return false;
}

bool WorkerThread::enter_cv_block() {
    // Standalone workers (no owner_ scheduler) use a simple timed sleep
    // without the full lost-wakeup protocol.  Work pushed while sleeping is
    // found on the next loop iteration via try_find_and_process_work().
    if (!owner_) {
        // Double-check for work that arrived before sleeping.
        {
            WorkItem item;
            if (pop(item)) {
                process_work_item(item);
                return true;
            }
        }
        // Only count escalation and flag CV-model after the double-check
        // confirms we are actually going to sleep.
        diag_cv_escalations_.fetch_add(1, std::memory_order_relaxed);
        in_cv_model_.store(true, std::memory_order_relaxed);
        // Sleep in short intervals so stop() can interrupt without blocking
        // thread_.join() for the full duration.
        constexpr auto kStandaloneSlice = std::chrono::milliseconds(10);
        auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
        while (std::chrono::steady_clock::now() < deadline) {
            if (stop_requested_.load(std::memory_order_acquire) ||
                !running_.load(std::memory_order_acquire)) {
                break;
            }
            std::this_thread::sleep_for(kStandaloneSlice);
        }
        return false;
    }

    auto& ws = owner_->placement_workers()[config_.worker_index];

    // Advertise parking intent (seq_cst — lost-wakeup protocol in WorkerPark).
    uint32_t snap = ws.park.begin_park();

    // Double-check for work that arrived between last poll and begin_park().
    {
        WorkItem item;
        if (owner_->pop_local(item, config_.worker_index) || try_steal(item)) {
            ws.park.end_park();
            process_work_item(item);
            return true;
        }
    }

    diag_cv_escalations_.fetch_add(1, std::memory_order_relaxed);
    in_cv_model_.store(true, std::memory_order_relaxed);

    // Compute EDF-aware timeout (same logic as before).
    auto now = std::chrono::steady_clock::now();
    int64_t timeout_ns = 100'000'000LL; // 100 ms default
    int64_t edf_ns = owner_->edf_next_deadline();
    if (edf_ns != INT64_MAX) {
        int64_t now_ns = now.time_since_epoch().count();
        int64_t delta_ns = edf_ns - now_ns;
        if (delta_ns <= 0)
            delta_ns = 1'000'000;
        int64_t margin_ns = 1'000'000;
        timeout_ns = std::max(delta_ns - margin_ns, margin_ns);
        timeout_ns = std::min(timeout_ns, int64_t{100'000'000});
    } else {
        constexpr int64_t kBaseNs = 500'000'000LL;
        constexpr int64_t kMaxNs = 30'000'000'000LL;
        uint32_t c = consecutive_empty_wakes_.load(std::memory_order_relaxed);
        uint32_t shift = std::min(c, 9u);
        timeout_ns = std::min(kBaseNs << shift, kMaxNs);
    }

    ws.park.wait(snap, timeout_ns);
    ws.park.end_park();

    // Distinguish timeout vs notify by checking if seq changed.
    bool notified = (ws.park.seq.load(std::memory_order_relaxed) != snap);
    if (notified) {
        diag_cv_notify_wakes_.fetch_add(1, std::memory_order_relaxed);
        consecutive_empty_wakes_.store(0, std::memory_order_relaxed);
    } else {
        diag_cv_timeout_wakes_.fetch_add(1, std::memory_order_relaxed);
        consecutive_empty_wakes_.fetch_add(1, std::memory_order_relaxed);
    }
    return false;
}

void WorkerThread::thread_loop() {
    tl_current_worker_id = config_.worker_index;

    while (!stop_requested_.load(std::memory_order_acquire) &&
           running_.load(std::memory_order_acquire)) {
        // Check if paused (test harness)
        if (pause_handler_) {
            pause_handler_();
        }

        // Phase 1: Try to find and process work (local pop → steal).
        if (try_find_and_process_work()) {
            continue;
        }

        // Phase 2: Polling idle model (yield -> proportional sleep -> capped
        // sleep).
        if (try_poll_idle()) {
            continue;
        }

        // Phase 3: CV blocking model.
        // enter_cv_block() returns true if work was found during the
        // pre-sleep double-check (already processed, idle state reset).
        // Returns false after CV wait completed without finding work.
        // In the false case, DON'T reset idle_since_ — let it keep
        // tracking from the original idle start so the next
        // try_poll_idle() immediately re-enters CV.
        if (enter_cv_block()) {
            continue;
        }
        // CV wait completed without finding work.
        // idle_since_ retains its pre-CV value -> immediate re-entry to CV.
    }
}

bool WorkerThread::diag_is_in_cv_model() const {
    return in_cv_model_.load(std::memory_order_relaxed);
}

void WorkerThread::backoff(std::chrono::nanoseconds elapsed) {
    uint64_t ns = static_cast<uint64_t>(elapsed.count());

    // Stage 0: spin (yield) only when yield is effective and we're within
    // the spin threshold.
    if (ns < calibration_.spin_threshold_ns) {
        if (calibration_.yield_is_effective) {
            std::this_thread::yield();
        }
        // On platforms where yield is a no-op, don't busy-wait at all.
        return;
    }

    // Stage 1: proportional sleep for the first 1 ms of idle time.
    // Sleep for elapsed/4 so backoff ramps up but polls frequently enough
    // to catch bursty work.
    if (ns < 1'000'000) {
        uint64_t sleep_ns = ns / 4;
#ifdef __linux__
        // On Linux CFS, nanosleep() with durations below ~100 us often
        // busy-waits instead of context-switching the thread (the kernel
        // decides the overhead of descheduling exceeds the sleep duration).
        // Use a 500 us floor — above the context-switch threshold on all
        // modern kernels — so the thread actually deschedules during
        // backoff.  This avoids burning CPU in the polling phase.
        if (sleep_ns < 500'000) {
            sleep_ns = 500'000;
        }
#else
        // On macOS/Mach, thread_switch() deschedules the caller at any
        // duration, so the probed min_effective_sleep_ns is sufficient.
        if (sleep_ns < calibration_.min_effective_sleep_ns) {
            sleep_ns = calibration_.min_effective_sleep_ns;
        }
#endif
        std::this_thread::sleep_for(std::chrono::nanoseconds(sleep_ns));
        return;
    }

    // Stage 2: capped moderate sleep (500 us) for the remainder of the
    // polling budget.  Polls often enough to be responsive but avoids
    // burning CPU.
    std::this_thread::sleep_for(std::chrono::nanoseconds(500'000));
}

} // namespace hpactor::sched