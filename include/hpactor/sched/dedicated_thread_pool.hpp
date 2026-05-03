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

class DedicatedThreadPool {
  public:
    using WorkFn = std::function<void()>;

    explicit DedicatedThreadPool(uint32_t num_threads);
    ~DedicatedThreadPool();

    DedicatedThreadPool(const DedicatedThreadPool&) = delete;
    DedicatedThreadPool& operator=(const DedicatedThreadPool&) = delete;
    DedicatedThreadPool(DedicatedThreadPool&&) = delete;
    DedicatedThreadPool& operator=(DedicatedThreadPool&&) = delete;

    void start();
    void stop();

    // Thread-safe: enqueue work for an actor.
    // Work is distributed round-robin across pool workers.
    void enqueue(ActorId actor, WorkFn work);

    bool is_running() const {
        return running_.load(std::memory_order_acquire);
    }

    size_t pending() const;

    uint32_t num_threads() const {
        return num_threads_;
    }

  private:
    void worker_loop(uint32_t worker_id);

    struct WorkerState {
        std::mutex mutex;
        std::vector<WorkFn> queue;
    };

    uint32_t num_threads_;
    std::vector<std::thread> threads_;
    std::vector<std::unique_ptr<WorkerState>> workers_;
    std::atomic<bool> running_{false};
    std::atomic<uint32_t> next_worker_{0};
};

} // namespace hpactor::sched
