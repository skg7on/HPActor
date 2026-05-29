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

#include <atomic>
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

enum class PlacementResult : uint8_t {
    EnqueuedShared,
    EnqueuedDedicatedPool,
    SuppressedDedicatedThread,
    NoWorkers,
};

using DedicatedDispatch = std::function<void(const WorkItem&)>;

class WorkPlacementScheduler {
  public:
    WorkPlacementScheduler(uint32_t num_workers, uint32_t num_priorities);
    ~WorkPlacementScheduler();

    WorkPlacementScheduler(const WorkPlacementScheduler&) = delete;
    WorkPlacementScheduler& operator=(const WorkPlacementScheduler&) = delete;

    void set_metrics_ring_buffer(
        metrics::MpscRingBuffer<metrics::MetricEvent>* metrics) noexcept;
    void set_logger(log::Logger* logger) noexcept;

    PlacementResult
    enqueue_admitted(const WorkItem& item, uint8_t priority, bool workers_paused,
                     const DedicatedDispatch& dedicated_dispatch);

    bool pop_local(uint32_t worker_id, WorkItem& out);
    bool pop_edf(uint32_t worker_id, WorkItem& out);
    bool try_steal(uint32_t thief_worker_id, WorkItem& out);
    bool pop_any_for_test(WorkItem& out);
    bool pop_one_on_worker_for_test(uint32_t worker_id, WorkItem& out);
    bool take_pinned_for_test(ActorId actor, WorkItem& out, uint32_t& worker_id);

    void pin_actor_to_worker(ActorId actor, uint32_t worker_id);
    void unpin_actor(ActorId actor);
    void flush_pinned_to_shared();

    void register_dedicated_thread(ActorId actor, int cpu_affinity);
    void register_dedicated_pool(ActorId actor, uint32_t pool_size);
    void unregister_dedicated(ActorId actor);

    struct alignas(64) WorkerState {
        std::unique_ptr<ChaselevDeque<WorkItem>[]> queues;
        uint32_t index{0};
        EDFQueue edf_queue;
    };

    uint32_t worker_count() const noexcept {
        return num_workers_;
    }

    A2WS& a2ws() noexcept {
        return a2ws_;
    }

    std::vector<WorkerState>& workers() noexcept {
        return workers_;
    }

  private:
    struct DedicatedStorage;

    uint32_t choose_worker(ActorId actor, bool& is_pinned);
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
