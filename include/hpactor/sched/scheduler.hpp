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
#include <hpactor/metrics/metrics_event.hpp>
#include <hpactor/metrics/metrics_ring_buffer.hpp>
#include <hpactor/sched/a2ws.hpp>
#include <hpactor/sched/actor_execution_engine.hpp>
#include <hpactor/sched/actor_ready_gate.hpp>
#include <hpactor/sched/edf_queue.hpp>
#include <hpactor/sched/scheduler_interfaces.hpp>
#include <hpactor/sched/work_placement_scheduler.hpp>
#include <hpactor/sched/work_queue.hpp>
#include <hpactor/timer/calendar_queue.hpp>
#include <hpactor/timer/timer_plane.hpp>
#include <hpactor/timer/timer_stats_snapshot.hpp>
#include <hpactor/timer/timing_wheel.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <variant>
#include <vector>

namespace hpactor {

// Forward declare ActorSystem to avoid circular include
class ActorSystem;

namespace log {
class Logger;
} // namespace log

} // namespace hpactor

namespace hpactor::sched {

class DedicatedThreadPool; // forward decl
class WorkerThread;        // forward decl

/// \brief Timer backend implementation selector.
enum class TimerBackend : uint8_t {
    TimingWheel = 0,   ///< Hierarchical timing wheel (O(1) insert/cancel).
    CalendarQueue = 1, ///< Calendar queue (good for sparse timers).
    TimerPlane = 2,    ///< Sharded timer plane (per-worker shards).
};

/// \brief Result of draining the ready queue.
struct SchedulerDrainResult {
    size_t executed = 0; ///< Number of work items executed.
    bool idle = true;    ///< \c true if no more ready actors remain.
};

/// \brief Lightweight snapshot of a worker thread for CLI inspection.
struct WorkerSnapshot {
    uint16_t worker_index{0};
    uint64_t actors_executed{0};
    uint64_t steals_attempted{0};
    uint64_t steals_successful{0};
    bool is_idle{false};

    // Adaptive wakeup diagnostics
    uint64_t work_found{0};
    uint64_t idle_iters{0};
    uint64_t cv_escalations{0};
    uint64_t cv_notify_wakes{0};
    uint64_t cv_timeout_wakes{0};
    uint64_t thread_id{0};  ///< Hashed \c std::thread::id for display.
    std::string idle_model; ///< Current idle model: \c "polling" or \c "cv".

    // Adaptive backoff calibration diagnostics
    bool calibration_yield_effective{false};
    uint32_t calibration_min_sleep_ns{0};
    uint32_t consecutive_empty_wakes{0};
};

/// \brief Abstract interface for actor schedulers.
///
/// The scheduler is the core execution engine — it routes ready actors to
/// worker threads, manages timers, and supports dedicated-thread and
/// dedicated-pool execution contexts.
///
/// \note Thread safety: \c notify_ready(), \c notify_idle(), and timer
///       methods are safe from any thread. Worker control methods are
///       intended for test harness use.
class IScheduler : public IActorReadyNotifier,
                   public ITimerService,
                   public IActorYieldScheduler {
  public:
    virtual ~IScheduler() = default;

    virtual void set_metrics_ring_buffer(void* /*buf*/) {}
    virtual void set_logger(void* /*logger*/) noexcept {}

    /// \brief Start all worker threads.
    virtual void start() = 0;

    /// \brief Stop all worker threads and timer advancement.
    virtual void stop() = 0;

    /// \brief Notify the scheduler that an actor is ready to run.
    ///
    /// Safe from any thread, including I/O threads.
    /// \param[in] actor Actor ID.
    /// \param[in] priority 0–3 (0 = highest).
    /// \param[in] deadline_ns Absolute deadline in nanoseconds.
    virtual void
    notify_ready(ActorId actor, uint8_t priority, int64_t deadline_ns) = 0;

    /// \brief Notify that an actor has become idle (blocked on I/O, etc.).
    ///
    /// Safe from any thread.
    /// \param[in] actor Actor ID.
    virtual void notify_idle(ActorId actor) = 0;

    /// \brief Voluntarily yield — re-enqueue at the same priority for
    ///        cooperative multitasking.
    ///
    /// \param[in] actor Actor ID.
    /// \param[in] priority Current priority level.
    virtual void yield(ActorId actor, uint8_t priority) = 0;

    /// \brief Schedule a one-shot timer.
    ///
    /// \param[in] cb Callback invoked when the timer fires.
    /// \param[in] delay_ns Delay in nanoseconds from now.
    /// \return Handle that can be used to cancel.
    virtual TimerHandle schedule_after(timer_callback cb, int64_t delay_ns) = 0;

    /// \brief Schedule a recurring timer.
    ///
    /// \param[in] cb Callback invoked every \p interval_ns.
    /// \param[in] interval_ns Interval in nanoseconds.
    /// \return Handle that can be used to cancel.
    virtual TimerHandle schedule_every(timer_callback cb, int64_t interval_ns) = 0;

    /// \brief Cancel a previously scheduled timer.
    ///
    /// \param[in] handle Timer handle returned by \c schedule_after() or
    ///                   \c schedule_every().
    virtual void cancel_timer(TimerHandle handle) = 0;

    /// \brief Number of worker threads.
    virtual size_t worker_count() const = 0;

    /// \brief Snapshot of per-worker thread statistics.
    virtual std::vector<WorkerSnapshot> worker_snapshots() const {
        return {};
    }

    /// \brief Returns \c true while workers are running.
    virtual bool is_running() const = 0;

    /// \brief Register an actor that needs a dedicated OS thread.
    ///
    /// \param[in] actor Actor ID.
    /// \param[in] cpu_affinity -1 = no affinity, >= 0 = pin to specific core.
    virtual void register_dedicated_thread(ActorId actor, int cpu_affinity) = 0;

    /// \brief Register an actor that needs a dedicated worker pool.
    ///
    /// The scheduler creates or reuses a \c DedicatedThreadPool of the
    /// given size.
    /// \param[in] actor Actor ID.
    /// \param[in] pool_size Number of threads in the pool.
    virtual void register_dedicated_pool(ActorId actor, uint32_t pool_size) = 0;

    /// \brief Shutdown a dedicated execution context for an actor.
    ///
    /// \param[in] actor Actor ID.
    virtual void unregister_dedicated(ActorId actor) = 0;

    // ── Worker control (deterministic testing) ─────────────────────────────

    virtual void pause_workers() noexcept {}
    virtual void resume_workers() noexcept {}
    virtual bool workers_paused() const noexcept {
        return false;
    }
    virtual bool run_one_ready() {
        return false;
    }
    virtual SchedulerDrainResult drain_ready(size_t max_items) {
        SchedulerDrainResult result;
        for (size_t i = 0; i < max_items; ++i) {
            if (!run_one_ready()) {
                result.idle = true;
                return result;
            }
            ++result.executed;
        }
        result.idle = false;
        return result;
    }

    // ── Actor-to-worker pinning (deterministic test control) ──────────────

    /// \brief Pin an actor to a specific worker.
    ///
    /// When pinned, \c notify_ready() routes this actor to the specified
    /// worker instead of round-robin.  When workers are paused, the work
    /// item is placed in a side queue so \c run_actor() can execute it
    /// deterministically.
    virtual void pin_actor_to_worker(ActorId /*actor*/, uint32_t /*worker_id*/) {}

    /// \brief Remove worker pinning for an actor.
    virtual void unpin_actor(ActorId /*actor*/) {}

    /// \brief Execute exactly one pinned actor.
    ///
    /// The actor must have been pinned via \c pin_actor_to_worker() and
    /// workers must be paused.  Returns \c true if the actor was found
    /// and executed.
    virtual bool run_actor(ActorId /*actor*/) {
        return false;
    }

    /// \brief Execute one ready item from a specific worker.
    ///
    /// Workers must be paused.  Checks the worker's pinned-ready deque
    /// first, then its EDF and priority queues.  Returns \c true if an
    /// item was executed.
    virtual bool run_one_on_worker(uint32_t /*worker_id*/) {
        return false;
    }
};

/// \brief Work-stealing hybrid scheduler with priority queues.
///
/// Each worker thread owns a \c ChaselevDeque for local work and an
/// \c EDFQueue for deadline-ordered execution. When a worker's local
/// queues are empty it steals from other workers via A2WS (Adaptive
/// Two-Level Work Stealing).
///
/// Supports three timer backends: \c TimingWheel (default, O(1)
/// insert/cancel) and \c CalendarQueue.
///
/// \note Thread safety: The public interface is internally synchronized.
///       Worker threads run the \c worker_loop() which is not reentrant.
class HybridScheduler : public IScheduler {
  public:
    /// \brief Construct the scheduler.
    ///
    /// \param[in] system Owning \c ActorSystem.
    /// \param[in] num_workers Number of worker threads.
    /// \param[in] num_priorities Number of priority levels (default 4,
    ///                          priorities 0 to N-1, 0 = highest).
    /// \param[in] timer_backend Timer implementation to use.
    /// \param[in] start_paused Start with workers paused (for testing).
    explicit HybridScheduler(ActorSystem& system, uint32_t num_workers,
                             uint32_t num_priorities = 4,
                             TimerBackend timer_backend = TimerBackend::TimingWheel,
                             bool start_paused = false);
    ~HybridScheduler() override;

    HybridScheduler(const HybridScheduler&) = delete;
    HybridScheduler& operator=(const HybridScheduler&) = delete;
    HybridScheduler(HybridScheduler&&) = delete;
    HybridScheduler& operator=(HybridScheduler&&) = delete;

    void start() override;
    void stop() override;
    void notify_ready(ActorId actor, uint8_t priority, int64_t deadline_ns) override;
    void notify_ready_edf(ActorId actor, uint8_t priority,
                          int64_t deadline_ns) override;
    void notify_idle(ActorId actor) override;
    void yield(ActorId actor, uint8_t priority) override;
    bool is_running() const override {
        return running_.load(std::memory_order_acquire);
    }
    size_t worker_count() const override {
        return num_workers_;
    }
    TimerHandle schedule_after(timer_callback cb, int64_t delay_ns) override;
    TimerHandle schedule_every(timer_callback cb, int64_t interval_ns) override;
    void cancel_timer(TimerHandle handle) override;

    void register_dedicated_thread(ActorId actor, int cpu_affinity) override;
    void register_dedicated_pool(ActorId actor, uint32_t pool_size) override;
    void unregister_dedicated(ActorId actor) override;

    std::vector<WorkerSnapshot> worker_snapshots() const override;

    /// \brief Collect a snapshot of timer statistics from the active backend.
    ///
    /// For \c TimerPlane, collects per-shard metrics including pending,
    /// fired, late, and dropped counts.  For \c TimingWheel and
    /// \c CalendarQueue, returns an empty snapshot with zero shards.
    ///
    /// \return A populated \c TimerStatsSnapshot.
    TimerStatsSnapshot timer_snapshot() const;

    // Worker control (deterministic testing)
    void pause_workers() noexcept override;
    void resume_workers() noexcept override;
    bool workers_paused() const noexcept override;
    bool run_one_ready() override;
    SchedulerDrainResult drain_ready(size_t max_items) override;

    // Actor-to-worker pinning (deterministic test control)
    void pin_actor_to_worker(ActorId actor, uint32_t worker_id) override;
    void unpin_actor(ActorId actor) override;
    bool run_actor(ActorId actor) override;
    bool run_one_on_worker(uint32_t worker_id) override;

    /// \brief Try to steal work from another worker.
    ///
    /// Called when the worker's local queue is empty.
    /// \param[out] out Set to the stolen work item on success.
    /// \return \c true if work was successfully stolen.
    bool try_steal(WorkItem& out);

    /// \brief Pop work from the owning worker's local queues.
    ///
    /// Called by WorkerThread during the local-pop fast path.
    /// \param[out] out Set to the popped work item on success.
    /// \param[in] worker_id Owning worker index.
    /// \return \c true if work was available.
    bool pop_local(WorkItem& out, uint32_t worker_id);

    /// \brief Return the earliest EDF deadline across all workers.
    ///
    /// Peeks at every worker's EDF queue without popping.  Used by the
    /// worker blocking-wakeup path to compute a safe CV timeout that
    /// guarantees deadline-sensitive work is stolen before it expires.
    ///
    /// \return Earliest deadline in nanoseconds, or \c INT64_MAX if all
    ///         EDF queues are empty.
    int64_t edf_next_deadline() noexcept;

    /// \brief Return a reference to the placement layer's worker state vector.
    ///
    /// Used by WorkerThread to access per-worker CV fields for adaptive
    /// blocking-wakeup.  Not intended for general use.
    std::vector<WorkPlacementScheduler::WorkerState>& placement_workers() noexcept {
        return placement_.workers();
    }

    /// \brief Read the current worker ID from thread-local storage.
    uint32_t current_worker_id() const;

    /// \brief Execute an actor from a work item.
    ///
    /// Handles coroutine resumption when available.
    void execute_actor(const WorkItem& item);

    /// \brief Schedule a timer with nanosecond resolution.
    ///
    /// \param[in] delay_ns Delay in nanoseconds from now.
    /// \param[in] callback Callback to invoke when the timer fires.
    /// \return Timer identifier for cancellation.
    uint64_t schedule_timer(int64_t delay_ns, timer_callback callback);

    /// \brief Advance time and process expired timers.
    ///
    /// Called by the timer advancement thread.
    /// \param[in] now_ns Current time in nanoseconds.
    void advance_time(int64_t now_ns);

  private:
    bool try_admit_ready(ActorId actor) noexcept;
    bool try_mark_yield_ready(ActorId actor) noexcept;
    void enqueue_admitted(const WorkItem& item, uint8_t priority);

    void wait_if_paused(uint32_t worker_id);
    bool pop_any_ready(WorkItem& out);
    bool pop_edf(WorkItem& out, uint32_t worker_id);
    void mark_dispatch_begin() noexcept;
    void mark_dispatch_end() noexcept;

    ActorSystem& system_;
    ActorReadyGate ready_gate_;
    WorkPlacementScheduler placement_;
    ActorExecutionEngine executor_;
    uint32_t num_workers_;
    std::atomic<bool> running_{false};
    std::vector<std::unique_ptr<WorkerThread>> worker_threads_;

    std::variant<TimingWheel, CalendarQueue, TimerPlane> timer_backend_;

    void set_metrics_ring_buffer(void* buf) noexcept override {
        metrics_ring_buffer_ =
            static_cast<metrics::MpscRingBuffer<metrics::MetricEvent>*>(buf);
    }

    void set_logger(void* logger) noexcept override {
        logger_ = static_cast<log::Logger*>(logger);
    }

    std::unordered_map<uint64_t, std::shared_ptr<std::atomic<bool>>> recurring_cancellations_;
    std::mutex cancellation_mutex_;

    /// Wakeup mechanism for the timer thread.
    /// schedule_after() notifies this when a new timer is registered so
    /// the timer thread can re-evaluate next_deadline() immediately.
    std::condition_variable timer_wakeup_cv_;
    std::mutex timer_wakeup_mutex_;

    metrics::MpscRingBuffer<metrics::MetricEvent>* metrics_ring_buffer_{nullptr};

    log::Logger* logger_{nullptr};

    std::atomic<bool> workers_paused_{false};
    std::atomic<uint32_t> active_worker_dispatches_{0};
    std::atomic<uint32_t> parked_worker_count_{0};
    std::mutex worker_control_mutex_;
    std::condition_variable worker_control_cv_;

    std::thread timer_thread_;
};

} // namespace hpactor::sched
