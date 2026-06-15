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

// Thread-local pointer to the current worker's frame pool
thread_local CoroutineFramePool* tl_frame_pool = nullptr;

// Thread-local worker ID (declared in scheduler.cpp, used by placement layer)
extern thread_local uint32_t tl_current_worker_id;

// ── Platform-specific backoff constants ──────────────────────────────
//
// Shared between thread_loop() and backoff().
//
// Linux:   sched_yield rotates the CFS run-queue but doesn't sleep —
//          the caller is immediately rescheduled if no other thread is
//          waiting.  Use 0 yields — escalate through nanosleep directly.
//
// macOS:   sched_yield uses Mach thread_switch, which actually yields
//          the CPU to other runnable threads.  Keep the original 4
//          yields that tested at near-zero CPU on macOS ARM64.
//
// kBackoffSleepIters = 4 on both platforms: 10+20+50+100 = 180 µs of
// nanosleep before CV entry — long enough for work to arrive naturally,
// short enough to reach deep sleep between timer bursts.

#if defined(__linux__)
constexpr uint32_t kBackoffYieldIters = 0;
constexpr uint32_t kBackoffSleepIters = 4;
#elif defined(__APPLE__)
constexpr uint32_t kBackoffYieldIters = 4;
constexpr uint32_t kBackoffSleepIters = 4;
#else
constexpr uint32_t kBackoffYieldIters = 0;
constexpr uint32_t kBackoffSleepIters = 4;
#endif

// kPollThreshold = total idle iterations before escalating to CV blocking.
constexpr uint32_t kPollThreshold = kBackoffYieldIters + kBackoffSleepIters;

WorkerThread::WorkerThread(const Config& config)
    : config_(config), local_queue_(config.priority_levels) {
    // Initialize calibration: use test override, shared probe result, or
    // defaults.  The calibration is copied so the override pointer can be
    // cleared or reused for subsequent workers.
    if (test_calibration_override_) {
        calibration_ = *test_calibration_override_;
    } else {
        calibration_ = shared_calibration_;
    }

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
    reset_backoff();
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
    // Standalone workers (no owner_ scheduler) stay in polling indefinitely;
    // attached workers escalate to CV blocking after kPollThreshold idle
    // iterations.  See platform-specific constants at the top of this file.
    if (!owner_ ||
        backoff_counter_.load(std::memory_order_relaxed) < kPollThreshold) {
        diag_idle_iters_.fetch_add(1, std::memory_order_relaxed);
        increment_donations();
        backoff();
        return true; // continue polling
    }
    return false; // escalate to CV blocking
}

bool WorkerThread::enter_cv_block() {
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

        // Phase 2: Polling idle model (yield → exponential backoff).
        // Standalone workers stay here; attached workers escalate after
        // kPollThreshold idle iterations.
        if (try_poll_idle()) {
            continue;
        }

        // Phase 3: CV blocking model with EDF-aware timeout and
        // lost-wakeup double-check protocol.
        if (enter_cv_block()) {
            continue;
        }
        reset_backoff();
    }
}

bool WorkerThread::diag_is_in_cv_model() const {
    return backoff_counter_.load(std::memory_order_relaxed) >= kPollThreshold;
}

void WorkerThread::backoff() {
    // See kBackoffYieldIters at the top of this file for the per-platform
    // yield threshold (0 on Linux, 4 on macOS).
    uint32_t c = backoff_counter_.fetch_add(1, std::memory_order_relaxed);

    if (c < kBackoffYieldIters) {
        std::this_thread::yield();
        return;
    }

    // Exponential backoff: 10us * 2^(c - kBackoffYieldIters), capped at
    // 50ms.  Cap the shift at 28 to avoid unsigned overflow (10u << 31
    // wraps to 0 on 32-bit, producing sleep_for(0us) which spins the core
    // at 100%).  The std::min at 50ms provides the effective backoff
    // ceiling — the shift ramps through it (10u << 13 = 81,920us → capped
    // to 50,000).
    uint32_t shift = (c - kBackoffYieldIters > 28) ? 28u : (c - kBackoffYieldIters);
    uint32_t backoff_us = 10u << shift;
    backoff_us = std::min(backoff_us, 50000u);
    std::this_thread::sleep_for(std::chrono::microseconds(backoff_us));
}

} // namespace hpactor::sched