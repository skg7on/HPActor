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

#include <hpactor/actor/actor_fwd.hpp>
#include <hpactor/coroutine/coroutine_frame_pool.hpp>
#include <hpactor/sched/work_queue.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <pthread.h>
#include <thread>
#include <vector>

namespace hpactor::mem {
class ThreadLocalAllocator;
}
namespace hpactor::sched {

// Forward declaration
class HybridScheduler;

// -----------------------------------------------------------------------------
// WorkerThread: per-thread worker for work-stealing scheduler
// -----------------------------------------------------------------------------
// Each worker has:
// - A local MultiPriorityWorkQueue for actor messages
// - Thread ID for identification
// - State flags for scheduling decisions
//
// Work-stealing strategy: Adaptive Work Stealing (AWS)
// - Each worker maintains a "donation" counter
// - When donation threshold exceeded, worker becomes a thief
// - Victims selected via per-worker round-robin pointer
// -----------------------------------------------------------------------------
class WorkerThread {
  public:
    struct Config {
        uint32_t worker_index = 0;
        uint32_t priority_levels = 4;
        uint32_t steal_threshold = 10;  // attempts before becoming active thief
        uint32_t victim_scan_limit = 4; // max victims to scan per steal attempt
        bool enable_thread_allocator = true; // set false for scheduler workers
    };

    /// \brief Whether the worker has escalated to the CV-blocking idle model.
    ///
    /// Returns \c true when \c backoff_counter_ >= \c kPollThreshold
    /// (platform-specific: 4 iters on Linux, 8 on macOS).
    /// Implemented in worker_thread.cpp to access the file-scoped constant.
    bool diag_is_in_cv_model() const;

    explicit WorkerThread(const Config& config);
    ~WorkerThread();

    WorkerThread(const WorkerThread&) = delete;
    WorkerThread& operator=(const WorkerThread&) = delete;
    WorkerThread(WorkerThread&&) = delete;
    WorkerThread& operator=(WorkerThread&&) = delete;

    // Start the worker thread
    void start();

    // Stop the worker thread
    void stop();

    // Enqueue work to this worker (push_bottom - owner operation)
    void push(uint8_t priority, WorkItem item);

    // Try to pop from local queue (pop_bottom - owner operation)
    bool pop(WorkItem& out);

    // Try to steal from this worker (steal_top - thief operation)
    bool steal(WorkItem& out);

    // Work processor callback — invoked for each work item.
    // When set, thread_loop() calls this instead of processing locally.
    using WorkProcessor = std::function<void(const WorkItem&)>;
    void set_work_processor(WorkProcessor proc) {
        processor_ = std::move(proc);
    }

    // Pause handler — blocks until the worker should proceed.
    // Used by test harness to pause/resume workers deterministically.
    using PauseHandler = std::function<void()>;
    void set_pause_handler(PauseHandler handler) {
        pause_handler_ = std::move(handler);
    }

    // Worker index
    uint32_t index() const {
        return config_.worker_index;
    }

    // Approximate queue depth
    size_t depth() const;

    // Check if running
    bool is_running() const {
        return running_.load(std::memory_order_acquire);
    }

    /// \brief Native OS thread identifier for CLI introspection.
    ///
    /// On Apple platforms, uses \c pthread_threadid_np to obtain a compact
    /// 64-bit integral ID.  On other platforms, falls back to a hash of
    /// \c std::thread::id (which is already a small integer on Linux).
    uint64_t thread_id() const {
#ifdef __APPLE__
        uint64_t tid = 0;
        // const_cast: native_handle() is logically const but std::thread
        // does not mark it so (known C++ standard library limitation).
        pthread_threadid_np(const_cast<std::thread&>(thread_).native_handle(), &tid);
        return tid;
#else
        return std::hash<std::thread::id>{}(thread_.get_id());
#endif
    }

    // For scheduling coordination: donation count for work-stealing decisions
    uint64_t donation_count() const {
        return donation_count_.load(std::memory_order_relaxed);
    }
    void increment_donations() {
        donation_count_.fetch_add(1, std::memory_order_relaxed);
    }

    // Coroutine frame pool integration
    // Acquire a coroutine frame from the pool (for blocking operations)
    CoroutineFramePool::Frame* acquire_frame();
    void release_frame(CoroutineFramePool::Frame* frame);

    // Set the frame pool (called by scheduler)
    void set_frame_pool(CoroutineFramePool* pool) {
        frame_pool_ = pool;
    }

    // Set the owner scheduler (for A2WS-based stealing)
    void set_owner(HybridScheduler* owner) {
        owner_ = owner;
    }

    // Set the fault controller for per-thread fault injection (may be nullptr)
    void set_fault_controller(void* fc) {
        fault_controller_ = fc;
    }

    // Per-thread memory allocator accessor
    mem::ThreadLocalAllocator* allocator() {
        return allocator_;
    }

    // Try to steal work using A2WS victim selection
    bool try_steal(WorkItem& out);

    // ── Diagnostic accessors ────────────────────────────────────────
    uint64_t diag_work_found() const {
        return diag_work_found_.load(std::memory_order_relaxed);
    }
    uint64_t diag_idle_iters() const {
        return diag_idle_iters_.load(std::memory_order_relaxed);
    }
    uint64_t diag_cv_escalations() const {
        return diag_cv_escalations_.load(std::memory_order_relaxed);
    }
    uint64_t diag_cv_notify_wakes() const {
        return diag_cv_notify_wakes_.load(std::memory_order_relaxed);
    }
    uint64_t diag_cv_timeout_wakes() const {
        return diag_cv_timeout_wakes_.load(std::memory_order_relaxed);
    }
    /// Current backoff counter.  Use \c diag_is_in_cv_model() to check
    /// whether the worker has escalated to CV-blocking idle.
    uint32_t diag_backoff_counter() const {
        return backoff_counter_.load(std::memory_order_relaxed);
    }

  private:
    void thread_loop();
    void backoff();

    void reset_backoff() {
        backoff_counter_.store(0, std::memory_order_relaxed);
    }

    Config config_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};

    // Owner scheduler (for A2WS access)
    HybridScheduler* owner_{nullptr};

    // Local priority queue for this worker
    MultiPriorityWorkQueue local_queue_;

    // Donation counter for adaptive stealing
    std::atomic<uint64_t> donation_count_{0};

    // Per-thread memory allocator
    mem::ThreadLocalAllocator* allocator_{nullptr};

    // Coroutine frame pool
    CoroutineFramePool* frame_pool_{nullptr};

    // Fault controller for per-thread fault injection (may be nullptr)
    void* fault_controller_{nullptr};

    // Pluggable work processor (set by scheduler)
    WorkProcessor processor_;

    // Pluggable pause handler (set by scheduler for test harness)
    PauseHandler pause_handler_;

    /// \brief Backoff counter for adaptive idle polling.
    ///
    /// Reset to 0 when work is found so the worker stays responsive;
    /// increments on each idle iteration to ramp sleep duration.
    /// Atomic so \c diag_is_in_cv_model() and \c diag_backoff_counter()
    /// can safely read it from CLI / metrics threads.
    std::atomic<uint32_t> backoff_counter_{0};

    // ── Diagnostic counters (exposed via WorkerSnapshot) ────────────
    std::atomic<uint64_t> diag_work_found_{0};
    std::atomic<uint64_t> diag_idle_iters_{0};
    std::atomic<uint64_t> diag_cv_escalations_{0};
    std::atomic<uint64_t> diag_cv_notify_wakes_{0};
    std::atomic<uint64_t> diag_cv_timeout_wakes_{0};
};

} // namespace hpactor::sched