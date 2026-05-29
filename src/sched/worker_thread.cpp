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

#include <hpactor/mem/memory_config.hpp>
#include <hpactor/mem/thread_local_allocator.hpp>
#include <hpactor/sched/scheduler.hpp>
#include <hpactor/sched/worker_thread.hpp>

#include <chrono>
#include <thread>

namespace hpactor::sched {

// Thread-local pointer to the current worker's frame pool
thread_local CoroutineFramePool* tl_frame_pool = nullptr;

WorkerThread::WorkerThread(const Config& config)
    : config_(config), local_queue_(config.priority_levels) {
    allocator_ = new mem::ThreadLocalAllocator();
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
        thread_loop();
    });
}

void WorkerThread::stop() {
    stop_requested_.store(true, std::memory_order_release);
    running_.store(false, std::memory_order_release);
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

void WorkerThread::process(const WorkItem& item) {
    // Process the actor - actual implementation would call actor's receive
    // This is a placeholder that would be wired to ActorSystem
    (void)item;
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
    if (!owner_) {
        return false;
    }

    uint32_t my_id = config_.worker_index;

    // Try up to victim_scan_limit victims via the placement scheduler.
    for (uint32_t attempt = 0; attempt < config_.victim_scan_limit; ++attempt) {
        uint32_t victim_idx = owner_->placement_.a2ws().get_victim(my_id);

        if (victim_idx >= owner_->placement_.worker_count()) {
            break;
        }

        auto& workers = owner_->placement_.workers();
        auto& victim = workers[victim_idx];

        // Try EDF queue first
        if (victim.edf_queue.pop(out)) {
            owner_->placement_.a2ws().record_steal(my_id, victim_idx);
            return true;
        }

        // Try each priority level
        for (uint32_t p = 0; p < 4; ++p) {
            if (victim.queues[p].steal_top(out)) {
                owner_->placement_.a2ws().record_steal(my_id, victim_idx);
                return true;
            }
        }

        owner_->placement_.a2ws().record_attempt(my_id, victim_idx, false);
    }
    return false;
}

void WorkerThread::thread_loop() {
    while (!stop_requested_.load(std::memory_order_acquire) &&
           running_.load(std::memory_order_acquire)) {
        WorkItem item;

        // Try to pop from local queue first (owner pop - fast path)
        if (pop(item)) {
            process(item);
            continue;
        }

        // Local empty - try stealing from another worker
        if (try_steal(item)) {
            process(item);
            continue;
        }

        // No work available - mark as donation candidate and backoff
        increment_donations();
        backoff();
    }
}

void WorkerThread::backoff() {
    static thread_local uint32_t count = 0;
    uint32_t c = count++;

    if (c < 4) {
        std::this_thread::yield();
    } else {
        uint32_t backoff_us = std::min<uint32_t>(1024u, 10u << (c - 4));
        std::this_thread::sleep_for(std::chrono::microseconds(backoff_us));
    }
}

} // namespace hpactor::sched