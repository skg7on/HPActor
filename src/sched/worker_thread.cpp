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

#include <hpactor/sched/worker_thread.hpp>

namespace hpactor::sched {

WorkerThread::WorkerThread(const Config& config)
    : config_(config), local_queue_(config.priority_levels) {}

WorkerThread::~WorkerThread() {
    stop();
}

void WorkerThread::start() {
    if (running_.load(std::memory_order_acquire)) {
        return;
    }
    running_.store(true, std::memory_order_release);
    stop_requested_.store(false, std::memory_order_release);
    thread_ = std::thread([this] { thread_loop(); });
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

void WorkerThread::thread_loop() {
    while (!stop_requested_.load(std::memory_order_acquire) && running_.load(std::memory_order_acquire)) {
        WorkItem item;

        // Try to pop from local queue first (owner pop - fast path)
        if (pop(item)) {
            process(item);
            continue;
        }

        // Local queue empty - this worker is a donation candidate
        increment_donations();

        // TODO: Work-stealing would be implemented here
        // - Select victim using round-robin
        // - Try to steal from victim's queue
        // - If steal succeeds, process the item

        // Backoff when no work available
        // In a real implementation, this would use exponential backoff
        // or yield/pause instructions
    }
}

} // namespace hpactor::sched