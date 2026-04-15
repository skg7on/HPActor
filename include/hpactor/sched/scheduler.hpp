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
#include <hpactor/actor/actor_fwd.hpp>

#include <atomic>
#include <cstdint>
#include <vector>

namespace hpactor {

// Forward declare ActorSystem to avoid circular include
class ActorSystem;

} // namespace hpactor

namespace hpactor::sched {

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

    // Enqueue an actor for processing at given priority
    virtual void enqueue(ActorId actor, uint8_t priority) = 0;

    // Enqueue an actor with deadline at given priority
    virtual void enqueue_deadline(ActorId actor, uint8_t priority, int64_t deadline_ns) = 0;

    // Check if scheduler is running
    virtual bool is_running() const = 0;

    // Number of worker threads
    virtual uint32_t num_workers() const = 0;
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
    void enqueue(ActorId actor, uint8_t priority) override;
    void enqueue_deadline(ActorId actor, uint8_t priority, int64_t deadline_ns) override;
    bool is_running() const override { return running_.load(std::memory_order_acquire); }
    uint32_t num_workers() const override { return num_workers_; }

    // Try to steal work from another worker (called when local queue is empty)
    bool try_steal(WorkItem& out);

    // Process one actor (called by worker loop)
    void process_actor(ActorId actor);

private:
    struct alignas(64) WorkerState {
        // Using unique_ptr array to avoid move semantics issues with ChaselevDeque
        std::unique_ptr<ChaselevDeque<WorkItem>[]> queues;
        uint32_t index;
    };

    void worker_loop(uint32_t worker_id);
    bool pop_local(WorkItem& out, uint32_t worker_id);

    ActorSystem& system_;
    uint32_t num_workers_;
    uint32_t num_priorities_;
    std::atomic<bool> running_{false};
    std::vector<WorkerState> workers_;

    // Victim selection: round-robin counter for work-stealing
    std::atomic<uint32_t> victim_counter_{0};
};

} // namespace hpactor::sched