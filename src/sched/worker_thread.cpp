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

namespace hpactor::sched {

// Thread-local pointer to the current worker's frame pool
thread_local CoroutineFramePool* tl_frame_pool = nullptr;

// Thread-local worker ID (declared in scheduler.cpp, used by placement layer)
extern thread_local uint32_t tl_current_worker_id;

// Backoff tuning constants — shared between thread_loop(), backoff(), and
// idle().  With kYieldIters=0, workers start directly with escalating
// nanosleeps instead of sched_yield(), avoiding the busy-poll trap on
// Linux where yield merely rotates the run-queue without sleeping.
// kPollThreshold = kYieldIters + kSleepIters (= 2): after two idle
// iterations (10µs, 20µs nanosleeps) the worker escalates to CV blocking.
static constexpr uint32_t kYieldIters = 0;
static constexpr uint32_t kSleepIters = 2;
static constexpr uint32_t kPollThreshold = kYieldIters + kSleepIters;

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
    thread_ = std::thread([this] {
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

void WorkerThread::thread_loop() {
    tl_current_worker_id = config_.worker_index;

    while (!stop_requested_.load(std::memory_order_acquire) &&
           running_.load(std::memory_order_acquire)) {
        // Check if paused (test harness)
        if (pause_handler_) {
            pause_handler_();
        }

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
            diag_work_found_.fetch_add(1, std::memory_order_relaxed);
            reset_backoff();
            if (processor_) {
                processor_(item);
            }
            continue;
        }

        // Local empty - try stealing from another worker
        if (try_steal(item)) {
            diag_work_found_.fetch_add(1, std::memory_order_relaxed);
            reset_backoff();
            if (processor_) {
                processor_(item);
            }
            continue;
        }

        // No work available — poll then escalate to CV blocking.
        if (idle(item)) {
            diag_work_found_.fetch_add(1, std::memory_order_relaxed);
            reset_backoff();
            if (processor_) {
                processor_(item);
            }
        }
    }
}

bool WorkerThread::idle(WorkItem& item) {
    // Poll phase: backoff with escalating nanosleeps.
    // Standalone workers (no owner_) never escalate to CV — they keep
    // polling indefinitely.
    if (!owner_ || backoff_counter_ < kPollThreshold) {
        diag_idle_iters_.fetch_add(1, std::memory_order_relaxed);
        increment_donations();
        backoff();
        return false;
    }

    // CV phase: escalate to condition-variable blocking.
    auto& ws = owner_->placement_workers()[config_.worker_index];
    ws.is_blocking_.store(true, std::memory_order_seq_cst);

    // Re-check for work before blocking — a producer may have enqueued
    // between the last poll check and setting is_blocking_.
    if (owner_->pop_local(item, config_.worker_index) || try_steal(item)) {
        ws.is_blocking_.store(false, std::memory_order_release);
        return true;
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
        // Wake 1 ms before the deadline to leave steal + dispatch
        // headroom.
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

    reset_backoff();
    return false;
}

void WorkerThread::backoff() {
    uint32_t c = backoff_counter_++;

    if (c < kYieldIters) {
        std::this_thread::yield();
        return;
    }

    // Escalating nanosleeps — architecture-independent: uses only standard
    // std::this_thread::sleep_for.  In normal operation the worker reaches
    // CV (kPollThreshold=2) after two idle iterations (10µs, 20µs), so the
    // 50µs/100µs entries are defence-in-depth for edge cases (e.g. standalone
    // workers without CV access, or delayed CV escalation).
    static constexpr uint32_t kSleepUs[] = {10, 20, 50, 100};
    uint32_t idx = c - kYieldIters;
    uint32_t sleep_us = (idx < 4) ? kSleepUs[idx] : kSleepUs[3]; // hold at
                                                                 // 100µs floor
    std::this_thread::sleep_for(std::chrono::microseconds(sleep_us));
}

} // namespace hpactor::sched