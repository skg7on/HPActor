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

#include <hpactor/sched/work_queue.hpp>
#include <hpactor/sched/edf_queue.hpp>
#include <hpactor/sched/a2ws.hpp>
#include <hpactor/sched/timing_wheel.hpp>
#include <hpactor/actor/actor_fwd.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>
#include <vector>

namespace hpactor {

// Forward declare ActorSystem to avoid circular include
class ActorSystem;

} // namespace hpactor

namespace hpactor::sched {

// -----------------------------------------------------------------------------
// TimerHandle and timer_callback types
// -----------------------------------------------------------------------------
struct TimerHandle {
    uint64_t id = 0;
    bool valid() const noexcept { return id != 0; }
};

using timer_callback = std::function<void()>;

// -----------------------------------------------------------------------------
// IScheduler: interface for actor schedulers
// -----------------------------------------------------------------------------
class IScheduler {
public:
    virtual ~IScheduler() = default;

    // Start the scheduler
    virtual void start() = 0;

    // Stop the scheduler
    virtual void stop() = 0;

    // Thread-safe; may be called from any thread including I/O threads
    // Notify scheduler that an actor is ready to run at given priority
    virtual void notify_ready(ActorId actor, uint8_t priority, int64_t deadline_ns) = 0;

    // Notify scheduler that an actor has become idle (blocked on I/O, etc.)
    virtual void notify_idle(ActorId actor) = 0;

    // Schedule a one-shot timer to fire after delay_ns
    virtual TimerHandle schedule_after(timer_callback cb, int64_t delay_ns) = 0;

    // Schedule a recurring timer to fire every interval_ns
    virtual TimerHandle schedule_every(timer_callback cb, int64_t interval_ns) = 0;

    // Cancel a previously scheduled timer
    virtual void cancel_timer(TimerHandle handle) = 0;

    // Number of worker threads
    virtual size_t worker_count() const = 0;

    // Check if scheduler is running
    virtual bool is_running() const = 0;
};

// -----------------------------------------------------------------------------
// HybridScheduler: work-stealing scheduler with priority queues
// -----------------------------------------------------------------------------
// Each worker has its own ChaseLev deque. Work-stealing is done by trying
// to pop from the target worker's deque when local work is exhausted.
// Priority levels 0-3 (0 = highest).
//
// Uses MultiPriorityWorkQueue for priority-based local enqueue.
// Work-stealing is attempted in round-robin order across workers.
// -----------------------------------------------------------------------------
class HybridScheduler : public IScheduler {
public:
    // num_priorities: number of priority levels (default 4, priorities 0..N-1)
    // ActorSystem reference is held for processing actors
    explicit HybridScheduler(ActorSystem& system, uint32_t num_workers, uint32_t num_priorities = 4);
    ~HybridScheduler() override;

    HybridScheduler(const HybridScheduler&) = delete;
    HybridScheduler& operator=(const HybridScheduler&) = delete;
    HybridScheduler(HybridScheduler&&) = delete;
    HybridScheduler& operator=(HybridScheduler&&) = delete;

    void start() override;
    void stop() override;
    void notify_ready(ActorId actor, uint8_t priority, int64_t deadline_ns) override;
    void notify_idle(ActorId actor) override;
    bool is_running() const override { return running_.load(std::memory_order_acquire); }
    size_t worker_count() const override { return num_workers_; }
    TimerHandle schedule_after(timer_callback cb, int64_t delay_ns) override;
    TimerHandle schedule_every(timer_callback cb, int64_t interval_ns) override;
    void cancel_timer(TimerHandle handle) override;

    // Try to steal work from another worker (called when local queue is empty)
    bool try_steal(WorkItem& out);

    // Process one actor (called by worker loop)
    void process_actor(ActorId actor);

    // Timing wheel integration
    // Schedule a timer to fire after delay_ns (in nanoseconds)
    // Returns timer ID that can be used to cancel
    uint64_t schedule_timer(int64_t delay_ns, TimingWheel::TimerCallback callback);

    // Advance time - processes expired timers
    void advance_time(int64_t now_ns);

private:
    struct alignas(64) WorkerState {
        // Using unique_ptr array to avoid move semantics issues with ChaselevDeque
        std::unique_ptr<ChaselevDeque<WorkItem>[]> queues;
        uint32_t index;
        EDFQueue edf_queue;  // For deadline-ordered work
    };

public:
    // A2WS access for WorkerThread
    A2WS& a2ws() { return a2ws_; }
    std::vector<WorkerState>& workers() { return workers_; }

    friend class WorkerThread;

    void worker_loop(uint32_t worker_id);
    bool pop_local(WorkItem& out, uint32_t worker_id);
    bool pop_edf(WorkItem& out, uint32_t worker_id);

    ActorSystem& system_;
    uint32_t num_workers_;
    uint32_t num_priorities_;
    std::atomic<bool> running_{false};
    std::vector<WorkerState> workers_;
    std::vector<std::thread> worker_threads_;

    // Adaptive two-level work stealing
    A2WS a2ws_;

    // Timing wheel for timer management
    TimingWheel timer_wheel_;
};

} // namespace hpactor::sched