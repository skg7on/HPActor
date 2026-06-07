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

#include <hpactor/types/types.hpp>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace hpactor::sched {

/// \brief A fixed-size pool of dedicated worker threads for dispatching
///        compute-bound (\c DenseComputingActor) work.
///
/// Each worker owns a mutex-protected queue. Work is distributed
/// round-robin across workers via an atomic counter. Workers spin on
/// the \c running_ flag, yielding when idle. The pool is non-copyable
/// and non-movable because it owns live \c std::thread objects.
///
/// \note Thread safety: \c enqueue() and \c pending() are safe to call
///       from any thread. \c start() and \c stop() must be sequenced
///       externally (typically called from the \c ActorSystem lifecycle).
class DedicatedThreadPool {
  public:
    /// \brief Work function type enqueued for execution.
    ///
    /// A nullary callable. \c nullptr functions are silently dropped
    /// by \c enqueue().
    using WorkFn = std::function<void()>;

    /// \brief Constructs the pool with a fixed number of worker slots.
    ///
    /// Creates \p num_threads \c WorkerState objects. The minimum is
    /// 1 worker (0 is clamped to 1). Workers are not started until
    /// \c start() is called.
    ///
    /// \param[in] num_threads Number of worker threads. Clamped to ≥1.
    explicit DedicatedThreadPool(uint32_t num_threads);

    /// \brief Stops all workers and joins their threads.
    ///
    /// Calls \c stop() before destruction. If the pool was never
    /// started, this is a no-op.
    ~DedicatedThreadPool();

    /// \brief Non-copyable.
    DedicatedThreadPool(const DedicatedThreadPool&) = delete;
    /// \brief Non-copyable.
    DedicatedThreadPool& operator=(const DedicatedThreadPool&) = delete;
    /// \brief Non-movable (owns live threads).
    DedicatedThreadPool(DedicatedThreadPool&&) = delete;
    /// \brief Non-movable (owns live threads).
    DedicatedThreadPool& operator=(DedicatedThreadPool&&) = delete;

    /// \brief Start all worker threads.
    ///
    /// Idempotent: if already running, this is a no-op. Sets the
    /// \c running_ flag and spawns \c num_threads_ OS threads, each
    /// executing \c worker_loop().
    ///
    /// \note Must be called before \c enqueue(). Typically invoked
    ///       during \c ActorSystem initialization.
    void start();

    /// \brief Stop all worker threads and join them.
    ///
    /// Idempotent: if not running, this is a no-op. Clears the
    /// \c running_ flag, drains all worker queues, and joins every
    /// thread. Any enqueued-but-not-executed work is discarded.
    ///
    /// \post All worker threads are joined. The pool can be restarted
    ///       with \c start().
    void stop();

    /// \brief Enqueue work for a specific actor.
    ///
    /// Work is distributed round-robin across workers via an atomic
    /// fetch-add on \c next_worker_. \p actor is reserved for future
    /// affinity placement but is currently unused.
    ///
    /// \param[in] actor The owning actor id (reserved for future affinity).
    /// \param[in] work The work function to execute. \c nullptr is
    ///                 silently dropped.
    /// \note Thread-safe: callable from any thread.
    // Thread-safe: enqueue work for an actor.
    // Work is distributed round-robin across pool workers.
    void enqueue(ActorId actor, WorkFn work);

    /// \brief Whether the pool is currently running.
    ///
    /// \return \c true if \c start() has been called and \c stop()
    ///         has not yet completed.
    /// \note Thread safety: lock-free atomic load with acquire ordering.
    bool is_running() const {
        return running_.load(std::memory_order_acquire);
    }

    /// \brief Total number of pending work items across all workers.
    ///
    /// Acquires each worker mutex in turn to sum queue sizes.
    ///
    /// \return The sum of all worker queue sizes.
    /// \note Thread safety: acquires each worker mutex. Callable from
    ///       any thread, but the result is a point-in-time snapshot.
    size_t pending() const;

    /// \brief Number of worker threads in this pool.
    ///
    /// \return The thread count specified at construction (clamped to ≥1).
    uint32_t num_threads() const {
        return num_threads_;
    }

  private:
    /// \brief Spin loop executed by each worker thread.
    ///
    /// Polls \c running_ and the worker's queue. Pops and executes one
    /// work item per iteration, yielding the thread when the queue is
    /// empty.
    ///
    /// \param[in] worker_id Index into \c workers_.
    void worker_loop(uint32_t worker_id);

    /// \brief Per-worker state: a mutex and a work queue.
    struct WorkerState {
        /// \brief Mutex protecting \c queue.
        std::mutex mutex;
        /// \brief FIFO work queue (vector used as a simple queue).
        std::vector<WorkFn> queue;
    };

    /// \brief Number of worker threads (clamped to ≥1 at construction).
    uint32_t num_threads_;

    /// \brief OS thread handles.
    ///
    /// Populated by \c start(), cleared by \c stop() after join.
    std::vector<std::thread> threads_;

    /// \brief Per-worker state (mutex + queue).
    ///
    /// Created at construction. Stable addresses across the pool's
    /// lifetime so worker loops can reference their slot by index.
    std::vector<std::unique_ptr<WorkerState>> workers_;

    /// \brief Whether the pool is running.
    ///
    /// Set by \c start() (release), cleared by \c stop() (release).
    /// Workers poll this with acquire ordering.
    std::atomic<bool> running_{false};

    /// \brief Atomic round-robin counter for work distribution.
    ///
    /// Incremented by \c enqueue() with relaxed ordering. The worker
    /// index is computed as \c fetch_add(1) % num_threads_.
    std::atomic<uint32_t> next_worker_{0};
};

} // namespace hpactor::sched
