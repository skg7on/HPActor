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

#include <hpactor/sched/dedicated_thread_pool.hpp>

#include <thread>

namespace hpactor::sched {

DedicatedThreadPool::DedicatedThreadPool(uint32_t num_threads)
    : num_threads_(num_threads == 0 ? 1 : num_threads) {
    for (uint32_t i = 0; i < num_threads_; ++i) {
        workers_.push_back(std::make_unique<WorkerState>());
    }
}

DedicatedThreadPool::~DedicatedThreadPool() {
    stop();
}

void DedicatedThreadPool::start() {
    if (running_.load(std::memory_order_acquire)) return;
    running_.store(true, std::memory_order_release);

    for (uint32_t i = 0; i < num_threads_; ++i) {
        threads_.emplace_back(&DedicatedThreadPool::worker_loop, this, i);
    }
}

void DedicatedThreadPool::stop() {
    if (!running_.load(std::memory_order_acquire)) return;
    running_.store(false, std::memory_order_release);

    // Wake all workers by clearing their queues
    for (auto& worker : workers_) {
        std::lock_guard<std::mutex> lock(worker->mutex);
        worker->queue.clear();
    }

    for (auto& t : threads_) {
        if (t.joinable()) t.join();
    }
    threads_.clear();
}

void DedicatedThreadPool::enqueue(ActorId /*actor*/, WorkFn work) {
    if (!work) return;

    // Round-robin distribution
    uint32_t idx = next_worker_.fetch_add(1, std::memory_order_relaxed) % num_threads_;
    auto& worker = workers_[idx];
    {
        std::lock_guard<std::mutex> lock(worker->mutex);
        worker->queue.push_back(std::move(work));
    }
}

size_t DedicatedThreadPool::pending() const {
    size_t total = 0;
    for (auto& worker : workers_) {
        std::lock_guard<std::mutex> lock(worker->mutex);
        total += worker->queue.size();
    }
    return total;
}

void DedicatedThreadPool::worker_loop(uint32_t worker_id) {
    auto& worker = workers_[worker_id];
    while (running_.load(std::memory_order_acquire)) {
        WorkFn work;
        {
            std::lock_guard<std::mutex> lock(worker->mutex);
            if (!worker->queue.empty()) {
                work = std::move(worker->queue.front());
                worker->queue.erase(worker->queue.begin());
            }
        }
        if (work) {
            work();
        } else {
            std::this_thread::yield();
        }
    }
}

} // namespace hpactor::sched
