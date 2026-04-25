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
#include <hpactor/sched/coroutine_frame_pool.hpp>
#include <hpactor/sched/work_queue.hpp>

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

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
    };

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

    // Process a single work item
    void process(const WorkItem& item);

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

    // Try to steal work using A2WS victim selection
    bool try_steal(WorkItem& out);

  private:
    void thread_loop();

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

    // Coroutine frame pool
    CoroutineFramePool* frame_pool_{nullptr};
};

} // namespace hpactor::sched