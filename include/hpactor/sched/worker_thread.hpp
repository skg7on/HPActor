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
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <pthread.h>
#include <thread>
#include <vector>

namespace hpactor::mem {
class ThreadLocalAllocator;
}
namespace hpactor::sched {

// Forward declaration
class HybridScheduler;

/// \brief OS scheduling characteristics measured at startup.
///
/// Populated once by a calibration probe that runs on the first worker
/// thread.  Values are immutable after construction.  Tests may inject a
/// fixed calibration via \c set_test_calibration() before \c start().
struct BackoffCalibration {
    /// \brief Whether \c sched_yield() actually yields the CPU.
    ///
    /// \c true on macOS/Mach where \c thread_switch() deschedules the
    /// caller.  \c false on Linux/CFS where yield merely rotates the
    /// run-queue and the caller is immediately rescheduled.
    bool yield_is_effective = false;

    /// \brief Minimum sleep duration the kernel can reliably honour.
    ///
    /// Derived from the knee in the actual-vs-requested sleep curve.
    /// Typically 10-50 us on modern Linux with hrtimers; 1-4 ms on
    /// older kernels or virtualized environments.
    uint32_t min_effective_sleep_ns = 50'000;

    /// \brief How long to spin (yield) before escalating to nanosleep.
    ///
    /// 0 when \c yield_is_effective is \c false (no point spinning on
    /// Linux).  ~20 us when yield actually deschedules the caller.
    uint32_t spin_threshold_ns = 0;

    /// \brief Total wall-clock idle time before escalating from polling
    ///        backoff to CV blocking.
    ///
    /// Default 1 ms (Linux-safe).  200 us on macOS where yield deschedules.
    /// Scales with timer granularity on coarse-grained kernels.
    uint32_t polling_budget_ns = 1'000'000;
};

/// \brief Per-thread worker for the work-stealing hybrid scheduler.
///
/// Each worker owns a dedicated OS thread that runs a work-processing loop.
/// The loop drains a local `MultiPriorityWorkQueue`, attempts to steal work
/// from sibling workers via the owner scheduler's A2WS victim selector, and
/// escalates from lightweight polling through exponential backoff to
/// condition-variable blocking when no work is available.
///
/// Workers integrate with the following scheduler subsystems:
/// - **Local queue:** `push()` (owner), `pop()` (owner), `steal()` (thief).
/// - **A2WS stealing:** `donation_count_` tracks steal attempts; when the
///   count exceeds `Config::steal_threshold`, the worker actively steals
///   via `try_steal()` using the scheduler's victim selector.
/// - **Adaptive idle escalation:** `backoff()` uses wall-clock idle time
///   with OS-calibrated sleep stages (yield → proportional sleep → capped
///   sleep).  After `polling_budget_ns` of idle time the worker enters
///   CV-based blocking with an EDF-aware timeout so it wakes before the
///   earliest deadline expires.
/// - **CV wakeup:** External enqueue paths check `is_blocking_` on the
///   placement-layer `WorkerState` and signal `sleep_cv_` to wake a
///   blocked worker immediately.
/// - **Coroutine frames:** Each worker owns a `CoroutineFramePool` for
///   stackful-coroutine frame allocation.
/// - **Per-thread allocation:** Optionally owns a `ThreadLocalAllocator`
///   for slab-cache-backed memory allocation confined to this thread.
///
/// \note **Thread safety:** The worker's own thread is the *owner* and
///       serializes all `push()`, `pop()`, and lifecycle operations.
///       Sibling workers may call `steal()` or `try_steal()` concurrently.
///       `start()`/`stop()` are externally synchronized by the scheduler.
class WorkerThread {
  public:
    /// \brief Per-worker configuration.
    ///
    /// Passed to the constructor; stored by value and immutable for the
    /// lifetime of the worker.
    struct Config {
        /// Index of this worker within the scheduler's worker array.
        uint32_t worker_index = 0;
        /// Number of priority levels in the local `MultiPriorityWorkQueue`.
        uint32_t priority_levels = 4;
        /// Steal-attempt threshold before the worker becomes an active thief.
        uint32_t steal_threshold = 10;
        /// Maximum victims to scan per steal attempt in A2WS.
        uint32_t victim_scan_limit = 4;
        /// When `true`, create a per-thread `ThreadLocalAllocator`.
        /// Set `false` for scheduler-owned workers that share a global
        /// allocator.
        bool enable_thread_allocator = true;
    };

    /// \brief Whether the worker has escalated to the CV-blocking idle model.
    ///
    /// Returns \c true when \c in_cv_model_ is set, which happens after the
    /// worker enters the CV sleep phase in \c enter_cv_block() and is cleared
    /// when work is found via \c process_work_item().
    bool diag_is_in_cv_model() const;

    /// \brief Construct a worker with the given configuration.
    ///
    /// Allocates the local priority queue and, if
    /// `config.enable_thread_allocator` is true, a per-thread slab
    /// allocator.  The OS thread is **not** started — call `start()` to
    /// launch it.
    ///
    /// \param[in] config Immutable per-worker configuration.
    explicit WorkerThread(const Config& config);

    /// \brief Destroy the worker.
    ///
    /// Calls `stop()` to join the OS thread (if running), then deletes
    /// the per-thread allocator (if any).
    ~WorkerThread();

    WorkerThread(const WorkerThread&) = delete;
    WorkerThread& operator=(const WorkerThread&) = delete;
    WorkerThread(WorkerThread&&) = delete;
    WorkerThread& operator=(WorkerThread&&) = delete;

    /// \brief Start the worker's OS thread.
    ///
    /// Idempotent — returns immediately if the worker is already running.
    /// The thread installs the frame pool, per-thread allocator, and fault
    /// controller (if configured) before entering `thread_loop()`.
    ///
    /// \note Must be called after `set_owner()`, `set_frame_pool()`, and
    ///       `set_work_processor()` for correct scheduler integration.
    void start();

    /// \brief Stop the worker's OS thread.
    ///
    /// Sets the stop-requested flag, wakes the worker if it is blocked on
    /// its sleep CV, then joins the thread.  Safe to call from any thread;
    /// the scheduler serializes stop across all workers during shutdown.
    void stop();

    /// \brief Push a work item into the local queue (owner operation).
    ///
    /// Only the worker's own thread or the scheduler may call this.
    ///
    /// \param[in] priority Priority level (0 = highest).
    /// \param[in] item Work item to enqueue.
    void push(uint8_t priority, WorkItem item);

    /// \brief Pop a work item from the local queue (owner operation).
    ///
    /// Only the worker's own thread may call this.
    ///
    /// \param[out] out Set to the popped item on success.
    /// \return `true` if an item was popped, `false` if the queue is empty.
    bool pop(WorkItem& out);

    /// \brief Steal a work item from this worker's local queue (thief
    ///        operation).
    ///
    /// Called by a sibling worker thread.  Steals from the highest-priority
    /// queue first.
    ///
    /// \param[out] out Set to the stolen item on success.
    /// \return `true` if an item was stolen, `false` if all queues are
    ///         empty.
    /// \note **Thread safety:** Lock-free and safe for concurrent thief
    ///       threads.
    bool steal(WorkItem& out);

    /// \brief Callback invoked for each work item processed by the worker.
    ///
    /// When set, `thread_loop()` calls this instead of processing work
    /// inline.  The scheduler installs this to route items through the
    /// actor execution engine.
    using WorkProcessor = std::function<void(const WorkItem&)>;

    /// \brief Install the work processor callback.
    ///
    /// Must be set before `start()` is called.
    ///
    /// \param[in] proc Callback invoked for each dequeued work item.
    void set_work_processor(WorkProcessor proc) {
        processor_ = std::move(proc);
    }

    /// \brief Callback invoked at the top of each loop iteration to pause
    ///        the worker.
    ///
    /// Used by the test harness to deterministically pause and resume
    /// workers for concurrency test scenarios.
    using PauseHandler = std::function<void()>;

    /// \brief Install the pause handler.
    ///
    /// When set, `thread_loop()` calls this handler at the top of each
    /// iteration.  The handler should block until the harness signals the
    /// worker to proceed.
    ///
    /// \param[in] handler Callback that blocks until the worker may proceed.
    void set_pause_handler(PauseHandler handler) {
        pause_handler_ = std::move(handler);
    }

    /// \brief Return this worker's index in the scheduler's worker array.
    ///
    /// \return The `worker_index` from the `Config` passed at construction.
    uint32_t index() const {
        return config_.worker_index;
    }

    /// \brief Approximate number of items in the local queue.
    ///
    /// \return Snapshot of queue depth across all priority levels.
    size_t depth() const;

    /// \brief Check whether the worker thread is running.
    ///
    /// \return `true` if `start()` has been called and `stop()` has not
    ///         yet joined the thread.
    bool is_running() const {
        return running_.load(std::memory_order_acquire);
    }

    /// \brief Native OS thread identifier for CLI introspection.
    ///
    /// On Apple platforms, uses \c pthread_threadid_np.  On Linux, returns the
    /// kernel TID captured via \c syscall(SYS_gettid) at thread start and
    /// stored in \c native_tid_.  Falls back to a \c native_handle() cast on
    /// other platforms.
    ///
    /// \return A platform-stable 64-bit thread identifier (0 if the worker
    ///         thread has not yet captured its TID).
    uint64_t thread_id() const {
#ifdef __APPLE__
        uint64_t tid = 0;
        // const_cast: native_handle() is logically const but std::thread
        // does not mark it so (known C++ standard library limitation).
        pthread_threadid_np(const_cast<std::thread&>(thread_).native_handle(), &tid);
        return tid;
#elif defined(__linux__)
        // glibc pthread_t from native_handle() is a struct pthread* cast to
        // unsigned long, not the kernel TID.  Use the real TID captured by the
        // worker thread at startup via syscall(SYS_gettid).
        return native_tid_.load(std::memory_order_relaxed);
#else
        // Fallback for other Unix-like platforms where pthread_t may be the
        // native thread identifier.
        return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(
            const_cast<std::thread&>(thread_).native_handle()));
#endif
    }

    /// \brief Current donation count for A2WS work-stealing decisions.
    ///
    /// Each idle loop iteration increments this counter.  When it exceeds
    /// `Config::steal_threshold`, the scheduler promotes this worker to an
    /// active thief.
    ///
    /// \return Snapshot of the donation counter.
    uint64_t donation_count() const {
        return donation_count_.load(std::memory_order_relaxed);
    }

    /// \brief Increment the donation counter (called each idle iteration).
    void increment_donations() {
        donation_count_.fetch_add(1, std::memory_order_relaxed);
    }

    /// \brief Acquire a coroutine frame from this worker's frame pool.
    ///
    /// \return A frame pointer, or `nullptr` if no frame pool is configured
    ///         or the pool is exhausted.
    /// \note Must be called from the worker's own thread.
    CoroutineFramePool::Frame* acquire_frame();
    void release_frame(CoroutineFramePool::Frame* frame);

    /// \brief Set the coroutine frame pool (called by the scheduler before
    ///        `start()`).
    ///
    /// \param[in] pool Pointer to the frame pool, or `nullptr` to disable.
    void set_frame_pool(CoroutineFramePool* pool) {
        frame_pool_ = pool;
    }

    /// \brief Set the owner scheduler for A2WS-based stealing and CV
    ///        wakeup.
    ///
    /// Must be set before `start()`.
    ///
    /// \param[in] owner Pointer to the owning `HybridScheduler`.
    void set_owner(HybridScheduler* owner) {
        owner_ = owner;
    }

    /// \brief Set the fault controller for per-thread fault injection.
    ///
    /// May be `nullptr` to disable fault injection for this worker.
    ///
    /// \param[in] fc Opaque pointer to a `FaultController`, or `nullptr`.
    void set_fault_controller(void* fc) {
        fault_controller_ = fc;
    }

    /// \brief Access the per-thread memory allocator.
    ///
    /// \return Pointer to the `ThreadLocalAllocator`, or `nullptr` if
    ///         `Config::enable_thread_allocator` was `false`.
    mem::ThreadLocalAllocator* allocator() {
        return allocator_;
    }

    /// \brief Try to steal work from a sibling worker using A2WS victim
    ///        selection.
    ///
    /// Delegates to the owner scheduler's `try_steal()`, which uses the
    /// A2WS victim selector to pick a target and attempt a `steal()`.
    ///
    /// \param[out] out Set to the stolen item on success.
    /// \return `true` if work was stolen.
    bool try_steal(WorkItem& out);

    // ── Diagnostic accessors ────────────────────────────────────────

    /// \brief Total work items found (local pop + steal) by this worker.
    uint64_t diag_work_found() const {
        return diag_work_found_.load(std::memory_order_relaxed);
    }
    /// \brief Idle loop iterations (polling, before CV escalation).
    uint64_t diag_idle_iters() const {
        return diag_idle_iters_.load(std::memory_order_relaxed);
    }
    /// \brief Number of times the worker escalated to CV blocking.
    uint64_t diag_cv_escalations() const {
        return diag_cv_escalations_.load(std::memory_order_relaxed);
    }
    /// \brief CV wakeups triggered by enqueue-side notifications.
    uint64_t diag_cv_notify_wakes() const {
        return diag_cv_notify_wakes_.load(std::memory_order_relaxed);
    }
    /// \brief CV wakeups triggered by EDF-aware timeout expiry.
    uint64_t diag_cv_timeout_wakes() const {
        return diag_cv_timeout_wakes_.load(std::memory_order_relaxed);
    }
    // ── Calibration (test injection + runtime access) ───────────────

    /// \brief Inject a fixed calibration for deterministic testing.
    ///
    /// When set (non-null), the startup probe is skipped and this
    /// calibration is used instead.  Call with \c nullptr to restore
    /// auto-calibration for subsequent workers.
    ///
    /// \note Not thread-safe — call before any worker \c start().
    static void set_test_calibration(const BackoffCalibration* cal) {
        test_calibration_override_ = cal;
    }

    /// \brief Read the calibration in effect for this worker.
    ///
    /// \return Immutable reference to the calibration used by this worker.
    const BackoffCalibration& calibration() const {
        return calibration_;
    }

    /// \brief Whether the OS yield operation actually deschedules the caller.
    bool diag_yield_is_effective() const {
        return calibration_.yield_is_effective;
    }
    /// \brief Measured kernel timer granularity in nanoseconds.
    uint32_t diag_min_sleep_ns() const {
        return calibration_.min_effective_sleep_ns;
    }
    /// \brief Consecutive CV wakeups that found no work (exponential
    ///        timeout level).
    uint32_t diag_consecutive_empty_wakes() const {
        return consecutive_empty_wakes_.load(std::memory_order_relaxed);
    }

  private:
    /// \brief Main work-processing loop running on the worker's OS thread.
    ///
    /// Orchestrates a three-phase escalation:
    /// 1. Try to find and process work (local pop → steal).
    /// 2. Polling idle model (yield → exponential backoff).
    /// 3. CV blocking model (EDF-aware sleep with lost-wakeup protection).
    void thread_loop();

    /// \brief Process a work item: bump diagnostic counter, reset backoff,
    ///        and invoke the work processor.
    ///
    /// Extracted to eliminate duplication across the local-pop, steal, and
    /// CV-double-check paths.
    ///
    /// \param[in] item The work item to process.
    void process_work_item(const WorkItem& item);

    /// \brief Try to find work from local queues or via work-stealing.
    ///
    /// Attempts local pop first (placement queues when attached to a
    /// scheduler, local queue when standalone), then falls back to
    /// work-stealing from sibling workers via A2WS.
    ///
    /// \return \c true if work was found and processed; \c false if no
    ///         work is currently available.
    /// \note When \c true is returned, the caller should \c continue the
    ///       main loop immediately.
    bool try_find_and_process_work();

    /// \brief Execute one iteration of the polling idle model.
    ///
    /// Tracks \c idle_since_ wall-clock timestamp.  Invokes \c backoff(elapsed)
    /// with OS-calibrated sleep stages.  Standalone workers (no owner
    /// scheduler) stay in this model indefinitely.  Attached workers escalate
    /// to CV blocking after \c polling_budget_ns of cumulative idle time.
    ///
    /// \return \c true if the worker should continue in the polling model
    ///         (caller \c continue s the main loop).
    /// \return \c false when the polling budget is exhausted and the
    ///         worker should escalate to CV blocking.
    bool try_poll_idle();

    /// \brief Enter the CV blocking model with EDF-aware timeout.
    ///
    /// Performs the lost-wakeup double-check protocol:
    /// 1. Set \c is_blocking_ (seq_cst) to advertise blocking intent.
    /// 2. Double-check local queues + steal — if work appears, clear the
    ///    flag and return \c true (caller continues the main loop).
    /// 3. Compute an EDF-aware CV timeout (capped at 100 ms) so the
    ///    worker wakes before the earliest deadline expires.
    /// 4. Block on \c sleep_cv_ until notified or timeout.
    ///
    /// \return \c true if work was found during the pre-sleep double-check
    ///         (already processed; caller should \c continue the main loop).
    /// \return \c false after the CV wait completed without finding work.
    ///         Caller should NOT reset \c idle_since_ — preserving it
    ///         lets the next \c try_poll_idle() immediately re-enter CV.
    bool enter_cv_block();

    /// \brief Adaptive idle backoff: yield -> proportional sleep -> capped
    /// sleep.
    ///
    /// Stages are determined by wall-clock \p elapsed time since idle start:
    /// - Stage 0 (< spin_threshold_ns): yield if effective, else no-op.
    /// - Stage 1 (< 1 ms): sleep for elapsed/4, floored at
    /// min_effective_sleep_ns.
    /// - Stage 2 (>= 1 ms): sleep for 500 us (capped).
    ///
    /// \param[in] elapsed Wall-clock time since the worker first became idle.
    void backoff(std::chrono::nanoseconds elapsed);

    Config config_;                    ///< Immutable per-worker configuration.
    std::thread thread_;               ///< OS thread handle.
    std::atomic<bool> running_{false}; ///< True after `start()`, cleared by
                                       ///< `stop()`.
    std::atomic<bool> stop_requested_{false}; ///< Set by `stop()` to signal
                                              ///< thread exit.

    /// Owner scheduler for A2WS victim selection and CV wakeup coordination.
    HybridScheduler* owner_{nullptr};

    /// Local multi-priority work queue (Chase-Lev deque per priority lane).
    MultiPriorityWorkQueue local_queue_;

    /// Donation counter incremented each idle iteration; drives A2WS
    /// active-thief promotion when it exceeds `Config::steal_threshold`.
    std::atomic<uint64_t> donation_count_{0};

    /// Per-thread slab allocator (owned, may be nullptr).
    mem::ThreadLocalAllocator* allocator_{nullptr};

    /// Coroutine frame pool for stackful-coroutine frame allocation.
    CoroutineFramePool* frame_pool_{nullptr};

    /// Opaque pointer to a `FaultController` for per-thread fault injection.
    void* fault_controller_{nullptr};

    /// Pluggable work processor callback (set by scheduler before `start()`).
    WorkProcessor processor_;

    /// Pluggable pause handler for test harness (blocking callback).
    PauseHandler pause_handler_;

    /// \brief Native OS thread ID captured at worker start.
    ///
    /// On Linux, written once by the worker thread during \c start() via
    /// \c syscall(SYS_gettid).  Read by \c thread_id() from any thread.
    std::atomic<uint64_t> native_tid_{0};

    // ── Adaptive backoff calibration ──────────────────────────────────

    /// \brief Calibration values used by this worker (copied from shared probe
    ///        or test override at startup).
    BackoffCalibration calibration_;

    /// \brief Wall-clock time when the worker first became idle.
    ///
    /// Default-constructed (epoch) means "not idle."  Set on the first idle
    /// iteration and cleared when work is found.
    std::chrono::steady_clock::time_point idle_since_{};

    /// \brief Number of consecutive CV timeout wakeups that found no work.
    ///
    /// Drives exponential growth of the safety-net CV timeout.  Reset to 0
    /// when work is found.  Atomic for snapshot reads from CLI/metrics threads.
    std::atomic<uint32_t> consecutive_empty_wakes_{0};

    /// \brief Whether the worker is currently inside the CV-blocking idle
    /// model.
    ///
    /// Set \c true when the worker actually sleeps on \c sleep_cv_, cleared
    /// when work is processed.  Used by \c diag_is_in_cv_model().
    std::atomic<bool> in_cv_model_{false};

    // ── Shared calibration state (one probe for all workers) ───────────

    /// \brief Shared result of the one-time calibration probe.
    static BackoffCalibration shared_calibration_;
    /// \brief One-time flag guarding the calibration probe.
    static std::once_flag calibration_once_;
    /// \brief Test override — when non-null, skip the probe and use this.
    static const BackoffCalibration* test_calibration_override_;

    // ── Diagnostic counters (exposed via WorkerSnapshot) ────────────
    std::atomic<uint64_t> diag_work_found_{0};
    std::atomic<uint64_t> diag_idle_iters_{0};
    std::atomic<uint64_t> diag_cv_escalations_{0};
    std::atomic<uint64_t> diag_cv_notify_wakes_{0};
    std::atomic<uint64_t> diag_cv_timeout_wakes_{0};
};

} // namespace hpactor::sched
