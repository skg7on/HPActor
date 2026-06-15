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
    cal.polling_budget_ns = std::max(10'000'000u, cal.min_effective_sleep_ns * 100);

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
    bool got_work = false;

    // Local pop: use placement queues when attached to scheduler,
    // local queue when standalone.
    if (owner_) {
        got_work = owner_->pop_local(item, config_.worker_index);
    } else {
        got_work = pop(item);
    }

    if (got_work) {
        process_work_item(item);
        return true;
    }

    // Local empty - try stealing from another worker via A2WS.
    if (try_steal(item)) {
        process_work_item(item);
        return true;
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
        in_cv_model_.store(true, std::memory_order_relaxed);
        diag_cv_escalations_.fetch_add(1, std::memory_order_relaxed);
        // Double-check for work that arrived before sleeping.
        {
            WorkItem item;
            if (pop(item)) {
                diag_work_found_.fetch_add(1, std::memory_order_relaxed);
                // Reset idle tracking — worker found work.
                idle_since_ = std::chrono::steady_clock::time_point{};
                consecutive_empty_wakes_.store(0, std::memory_order_relaxed);
                in_cv_model_.store(false, std::memory_order_relaxed);
                if (processor_)
                    processor_(item);
                return true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        return false;
    }

    auto& ws = owner_->placement_workers()[config_.worker_index];

    // Advertise blocking intent with seq_cst to prevent the enqueue path
    // from reordering its is_blocking_ load before this store (lost-wakeup
    // protocol).
    ws.is_blocking_.store(true, std::memory_order_seq_cst);

    // Double-check for work that may have arrived between our last poll and
    // setting is_blocking_.  If work is found, clear the flag and return
    // immediately.
    {
        WorkItem item;
        if (owner_->pop_local(item, config_.worker_index) || try_steal(item)) {
            diag_work_found_.fetch_add(1, std::memory_order_relaxed);
            ws.is_blocking_.store(false, std::memory_order_release);
            reset_backoff();
            if (processor_)
                processor_(item);
            return true; // caller continues the main loop
        }
    }

    diag_cv_escalations_.fetch_add(1, std::memory_order_relaxed);
    in_cv_model_.store(true, std::memory_order_relaxed);

    // Compute EDF-aware CV timeout.  Wake before the earliest deadline
    // expires so another worker can steal deadline work.
    auto now = std::chrono::steady_clock::now();
    auto timeout = std::chrono::milliseconds(100);
    int64_t edf_ns = owner_->edf_next_deadline();
    if (edf_ns != INT64_MAX) {
        int64_t now_ns = now.time_since_epoch().count();
        int64_t delta_ns = edf_ns - now_ns;
        if (delta_ns <= 0)
            delta_ns = 1'000'000; // overdue: 1 ms floor
        auto delta_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::nanoseconds(delta_ns));
        // Wake 1 ms before the deadline to leave steal + dispatch headroom.
        auto margin = std::chrono::milliseconds(1);
        timeout = (delta_ms > margin) ? (delta_ms - margin)
                                      : std::chrono::milliseconds(1);
        if (timeout > std::chrono::milliseconds(100))
            timeout = std::chrono::milliseconds(100);
    }

    std::unique_lock<std::mutex> lk(ws.sleep_mutex_);
    bool timed_out = !ws.sleep_cv_.wait_for(lk, timeout, [&] {
        return !ws.is_blocking_.load(std::memory_order_relaxed) ||
               stop_requested_.load(std::memory_order_relaxed) ||
               !running_.load(std::memory_order_relaxed);
    });
    if (timed_out) {
        diag_cv_timeout_wakes_.fetch_add(1, std::memory_order_relaxed);
    } else {
        diag_cv_notify_wakes_.fetch_add(1, std::memory_order_relaxed);
    }
    return false; // CV wait completed; caller resets backoff and loops
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
        if (sleep_ns < calibration_.min_effective_sleep_ns) {
            sleep_ns = calibration_.min_effective_sleep_ns;
        }
        std::this_thread::sleep_for(std::chrono::nanoseconds(sleep_ns));
        return;
    }

    // Stage 2: capped moderate sleep (500 us) for the remainder of the
    // polling budget.  Polls often enough to be responsive but avoids
    // burning CPU.
    std::this_thread::sleep_for(std::chrono::nanoseconds(500'000));
}

} // namespace hpactor::sched