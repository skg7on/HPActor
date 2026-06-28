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

#include <hpactor/log/logger.hpp>
#include <hpactor/metrics/metrics_event.hpp>
#include <hpactor/metrics/metrics_ring_buffer.hpp>
#include <hpactor/sched/a2ws.hpp>
#include <hpactor/sched/edf_queue.hpp>
#include <hpactor/sched/work_queue.hpp>
#include <hpactor/sched/worker_park.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace hpactor::sched {

class DedicatedThreadPool;

/// \brief Outcome of an \c enqueue_admitted() call.
enum class PlacementResult : uint8_t {
    EnqueuedShared,            ///< Work was placed on a shared worker queue.
    EnqueuedDedicatedPool,     ///< Work was dispatched to a dedicated pool.
    SuppressedDedicatedThread, ///< Actor has a dedicated thread — no shared
                               ///< placement.
    NoWorkers,                 ///< No worker threads configured.
};

/// \brief Callback invoked by the placement layer when a dedicated-pool
///        worker must execute a work item.
using DedicatedDispatch = std::function<void(const WorkItem&)>;

/// \brief M:N worker placement scheduler.
///
/// Owns worker queue state: Chase-Lev priority queues, EDF deadline queues,
/// A2WS victim selection, pinned-actor routing, and dedicated-thread/pool
/// dispatch. Receives already-admitted work items and routes them to the
/// appropriate worker or pool.
///
/// The placement layer never calls \c receive(), never resumes a coroutine,
/// and never interprets actor mode. It only moves \c WorkItem values
/// between queues and workers.
///
/// \note Thread safety: \c enqueue_admitted(), \c pin_actor_to_worker(),
///       \c unpin_actor(), and dedicated registration methods are safe from
///       any thread. \c pop_local() is called only by the owning worker.
///       \c try_steal() is safe from worker threads and test helpers.
class WorkPlacementScheduler {
  public:
    /// \brief Construct a placement scheduler.
    ///
    /// \param[in] num_workers Number of worker threads.
    /// \param[in] num_priorities Number of priority levels (0 = highest,
    ///                          N-1 = lowest).
    WorkPlacementScheduler(uint32_t num_workers, uint32_t num_priorities);
    ~WorkPlacementScheduler();

    WorkPlacementScheduler(const WorkPlacementScheduler&) = delete;
    WorkPlacementScheduler& operator=(const WorkPlacementScheduler&) = delete;

    /// \brief Set the metrics ring buffer for steal events.
    ///
    /// \param[in] metrics Pointer to a lock-free ring buffer, or \c nullptr
    ///                    to disable.
    void set_metrics_ring_buffer(
        metrics::MpscRingBuffer<metrics::MetricEvent>* metrics) noexcept;

    /// \brief Set the logger for debug events.
    ///
    /// \param[in] logger Pointer to a logger, or \c nullptr to disable.
    void set_logger(log::Logger* logger) noexcept;

    /// \brief Enqueue an already-admitted work item.
    ///
    /// Routes the item based on actor pinning, dedicated-thread/pool
    /// registration, and worker-pause state.
    ///
    /// \param[in] item Work item to enqueue.
    /// \param[in] priority Priority level (0 = highest).
    /// \param[in] workers_paused If \c true, pinned actors use the side
    ///                           deque for deterministic test control.
    /// \param[in] dedicated_dispatch Callback invoked when a dedicated-pool
    ///                               worker picks up the item.
    /// \return Where the item was placed.
    PlacementResult
    enqueue_admitted(const WorkItem& item, uint8_t priority, bool workers_paused,
                     const DedicatedDispatch& dedicated_dispatch);

    /// \brief Pop work from the owning worker's local queues.
    ///
    /// Checks EDF first, then priority queues highest-to-lowest.
    ///
    /// \param[in] worker_id Owning worker index.
    /// \param[out] out Set to the popped work item on success.
    /// \return \c true if work was available.
    bool pop_local(uint32_t worker_id, WorkItem& out);

    /// \brief Pop the earliest-deadline work from a worker's EDF queue.
    ///
    /// \param[in] worker_id Worker index.
    /// \param[out] out Set to the popped work item on success.
    /// \return \c true if an EDF item was available.
    bool pop_edf(uint32_t worker_id, WorkItem& out);

    /// \brief Try to steal work from another worker.
    ///
    /// Uses A2WS for adaptive victim selection. Emits steal metrics.
    ///
    /// \param[in] thief_worker_id Index of the stealing worker.
    /// \param[out] out Set to the stolen work item on success.
    /// \return \c true if work was successfully stolen.
    bool try_steal(uint32_t thief_worker_id, WorkItem& out);

    /// \brief Scan all workers for any ready work (test helper).
    ///
    /// Uses steal semantics so the calling thread is not required to be
    /// the owning worker.
    ///
    /// \param[out] out Set to the popped work item on success.
    /// \return \c true if work was found.
    bool pop_any_for_test(WorkItem& out);

    /// \brief Pop one work item from a specific worker, checking the
    ///        pinned side queue first (test helper).
    ///
    /// \param[in] worker_id Worker index.
    /// \param[out] out Set to the popped work item on success.
    /// \return \c true if work was found.
    bool pop_one_on_worker_for_test(uint32_t worker_id, WorkItem& out);

    /// \brief Take a pinned actor's work item from the side deque
    ///        (test helper).
    ///
    /// \param[in] actor Actor ID.
    /// \param[out] out Set to the work item on success.
    /// \param[out] worker_id Set to the pinned worker index on success.
    /// \return \c true if the actor had a queued work item.
    bool take_pinned_for_test(ActorId actor, WorkItem& out, uint32_t& worker_id);

    /// \brief Pin an actor to a specific worker for deterministic
    ///        execution.
    ///
    /// When workers are paused, pinned work goes to a side deque so
    /// \c run_actor() can drive execution step-by-step.
    ///
    /// \param[in] actor Actor ID.
    /// \param[in] worker_id Target worker index.
    void pin_actor_to_worker(ActorId actor, uint32_t worker_id);

    /// \brief Remove worker pinning for an actor.
    ///
    /// \param[in] actor Actor ID.
    void unpin_actor(ActorId actor);

    /// \brief Flush all pinned-ready side deques back to the shared worker
    ///        queues.
    ///
    /// Called by \c resume_workers() when transitioning from paused to
    /// running mode.
    void flush_pinned_to_shared();

    /// \brief Register an actor that needs a dedicated OS thread.
    ///
    /// \param[in] actor Actor ID.
    /// \param[in] cpu_affinity -1 = no affinity, >= 0 = pin to core.
    void register_dedicated_thread(ActorId actor, int cpu_affinity);

    /// \brief Register an actor that needs a dedicated worker pool.
    ///
    /// Creates or reuses a \c DedicatedThreadPool of the given size.
    ///
    /// \param[in] actor Actor ID.
    /// \param[in] pool_size Number of threads in the pool.
    void register_dedicated_pool(ActorId actor, uint32_t pool_size);

    /// \brief Remove dedicated execution registration for an actor.
    ///
    /// \param[in] actor Actor ID.
    void unregister_dedicated(ActorId actor);

    /// \brief Node in the lock-free shared-input stack (fallback when ring
    /// full).
    struct SharedInputNode {
        WorkItem item;
        uint8_t priority{0};
        std::atomic<SharedInputNode*> next{nullptr};
    };

    /// \brief Bounded MPSC inject ring — zero-allocation cross-thread
    /// injection.
    ///
    /// Producers claim slots via CAS on \c tail; the owner drains sequentially
    /// from \c head.  Returns \c false on full; callers fall back to
    /// \c SharedInputNode.  256 slots fit in 16 KB (L1-resident per worker).
    struct InjectRing {
        static constexpr uint32_t kCapacity = 256;
        static constexpr uint32_t kMask = kCapacity - 1;

        struct Slot {
            std::atomic<uint64_t> seq{0};
            WorkItem item{};
            uint8_t priority{0};
        };

        Slot slots[kCapacity];
        alignas(64) std::atomic<uint64_t> tail{0}; ///< Producers increment.
        alignas(64) uint64_t head{0};              ///< Owner-only consumer.

        InjectRing() noexcept {
            for (uint32_t i = 0; i < kCapacity; ++i)
                slots[i].seq.store(i, std::memory_order_relaxed);
        }

        /// \brief Try to push one item. Returns \c false if the ring is full.
        bool try_push(const WorkItem& item, uint8_t priority) noexcept {
            uint64_t pos = tail.load(std::memory_order_relaxed);
            for (;;) {
                Slot& slot = slots[pos & kMask];
                uint64_t seq = slot.seq.load(std::memory_order_acquire);
                int64_t diff =
                    static_cast<int64_t>(seq) - static_cast<int64_t>(pos);
                if (diff == 0) {
                    if (tail.compare_exchange_weak(pos, pos + 1,
                                                   std::memory_order_acq_rel,
                                                   std::memory_order_relaxed)) {
                        slot.item = item;
                        slot.priority = priority;
                        slot.seq.store(pos + 1, std::memory_order_release);
                        return true;
                    }
                } else if (diff < 0) {
                    return false; // full
                } else {
                    pos = tail.load(std::memory_order_relaxed);
                }
            }
        }

        /// \brief Pop one item (owner only). Returns \c false if empty.
        bool try_pop(WorkItem& out, uint8_t& priority) noexcept {
            Slot& slot = slots[head & kMask];
            if (slot.seq.load(std::memory_order_acquire) != head + 1)
                return false;
            out = slot.item;
            priority = slot.priority;
            slot.seq.store(head + kCapacity, std::memory_order_release);
            ++head;
            return true;
        }
    };

    /// \brief Per-worker queue and EDF state.
    struct alignas(64) WorkerState {
        std::unique_ptr<ChaselevDeque<WorkItem>[]> queues;
        uint32_t index{0};
        EDFQueue edf_queue;
        /// Fast-path zero-allocation inject ring.  Producers try this first;
        /// fall back to shared_input (SharedInputNode heap) when full.
        InjectRing inject_ring;
        /// Lock-free stack for cross-thread work submission (ring-full
        /// fallback).
        std::atomic<SharedInputNode*> shared_input{nullptr};

        /// Futex-based park (replaces is_blocking_ + sleep_mutex_ + sleep_cv_).
        WorkerPark park;

        /// Mutex serializing pushes into edf_queue from concurrent producer
        /// threads.  EDF items are infrequent by design, so a mutex is
        /// adequate — upgrade to lock-free if profiling shows contention.
        mutable std::mutex edf_push_mutex_;

        /// Wake a parked worker (lock-free, idempotent).
        void wake_if_blocking() {
            park.wake();
        }
    };

    /// \return Number of worker threads.
    uint32_t worker_count() const noexcept {
        return num_workers_;
    }

    /// \return Reference to the A2WS victim selector (for \c WorkerThread
    ///         use).
    A2WS& a2ws() noexcept {
        return a2ws_;
    }

    /// \return Reference to the worker state vector (for \c WorkerThread
    ///         use).
    std::vector<WorkerState>& workers() noexcept {
        return workers_;
    }

  private:
    struct DedicatedStorage;

    uint32_t choose_worker(const WorkItem& item, bool& is_pinned);
    void enqueue_shared(const WorkItem& item, uint8_t priority, uint32_t worker_id);
    void emit_steal_metric(const WorkItem& item, uint32_t from_worker);

    uint32_t num_workers_;
    uint32_t num_priorities_;
    std::vector<WorkerState> workers_;
    A2WS a2ws_;
    std::unique_ptr<DedicatedStorage> dedicated_;

    mutable std::mutex pinned_mutex_;
    std::unordered_map<ActorId, uint32_t> pinned_actors_;
    std::vector<std::deque<WorkItem>> pinned_ready_;

    metrics::MpscRingBuffer<metrics::MetricEvent>* metrics_ring_buffer_{nullptr};
    log::Logger* logger_{nullptr};
};

} // namespace hpactor::sched
