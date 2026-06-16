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

#include <hpactor/sched/work_placement_scheduler.hpp>

#include <hpactor/log/log_field.hpp>
#include <hpactor/log/logger.hpp>
#include <hpactor/sched/dedicated_thread_pool.hpp>

#include <climits>

namespace hpactor::sched {

extern thread_local uint32_t tl_current_worker_id;

struct WorkPlacementScheduler::DedicatedStorage {
    std::unordered_set<ActorId> dedicated_thread_actors_;
    std::unordered_map<ActorId, int> dedicated_thread_affinity_;
    std::mutex dedicated_mutex_;
    std::unordered_map<uint32_t, std::unique_ptr<DedicatedThreadPool>> dedicated_pools_;
    std::unordered_map<ActorId, uint32_t> actor_pool_map_;
};

WorkPlacementScheduler::WorkPlacementScheduler(uint32_t num_workers,
                                               uint32_t num_priorities)
    : num_workers_(num_workers), num_priorities_(num_priorities),
      workers_(num_workers), a2ws_(num_workers),
      dedicated_(std::make_unique<DedicatedStorage>()),
      pinned_ready_(num_workers) {
    for (uint32_t i = 0; i < num_workers_; ++i) {
        workers_[i].queues =
            std::make_unique<ChaselevDeque<WorkItem>[]>(num_priorities_);
        workers_[i].index = i;
    }
}

WorkPlacementScheduler::~WorkPlacementScheduler() = default;

void WorkPlacementScheduler::set_metrics_ring_buffer(
    metrics::MpscRingBuffer<metrics::MetricEvent>* metrics) noexcept {
    metrics_ring_buffer_ = metrics;
}

void WorkPlacementScheduler::set_logger(log::Logger* logger) noexcept {
    logger_ = logger;
}

uint32_t WorkPlacementScheduler::choose_worker(ActorId actor, bool& is_pinned) {
    is_pinned = false;
    {
        std::lock_guard<std::mutex> lock(pinned_mutex_);
        auto it = pinned_actors_.find(actor);
        if (it != pinned_actors_.end()) {
            is_pinned = true;
            return it->second % num_workers_;
        }
    }

    static std::atomic<uint32_t> rr_counter{0};
    return rr_counter.fetch_add(1, std::memory_order_relaxed) % num_workers_;
}

void WorkPlacementScheduler::enqueue_shared(const WorkItem& item,
                                            uint8_t priority, uint32_t worker_id) {
    auto& worker = workers_[worker_id];

    // EDF-scheduled items bypass the shared-input stack: push directly
    // into the EDF min-heap so deadline ordering is preserved without
    // an intermediate LIFO→FIFO reversal.
    if (item.edf_scheduled) {
        {
            std::lock_guard<std::mutex> lock(worker.edf_push_mutex_);
            worker.edf_queue.push(item.deadline_ns, item);
        }
        worker.wake_if_blocking();
        return;
    }

    // Existing path: priority-only items go to shared-input stack.
    (void)priority; // priority is applied when the owner drains into its deque
    auto* node = new SharedInputNode();
    node->item = item;
    node->priority = priority;
    SharedInputNode* old = worker.shared_input.load(std::memory_order_acquire);
    do {
        node->next.store(old, std::memory_order_relaxed);
    } while (!worker.shared_input.compare_exchange_weak(
        old, node, std::memory_order_release, std::memory_order_relaxed));

    worker.wake_if_blocking();
}

PlacementResult
WorkPlacementScheduler::enqueue_admitted(const WorkItem& item, uint8_t priority,
                                         bool workers_paused,
                                         const DedicatedDispatch& dedicated_dispatch) {
    {
        std::lock_guard<std::mutex> lock(dedicated_->dedicated_mutex_);
        if (dedicated_->dedicated_thread_actors_.find(item.actor) !=
            dedicated_->dedicated_thread_actors_.end()) {
            return PlacementResult::SuppressedDedicatedThread;
        }

        auto actor_pool = dedicated_->actor_pool_map_.find(item.actor);
        if (actor_pool != dedicated_->actor_pool_map_.end()) {
            auto pool = dedicated_->dedicated_pools_.find(actor_pool->second);
            if (pool != dedicated_->dedicated_pools_.end()) {
                auto* dedicated_pool = pool->second.get();
                dedicated_pool->enqueue(item.actor, [dedicated_dispatch, item]() {
                    dedicated_dispatch(item);
                });
                return PlacementResult::EnqueuedDedicatedPool;
            }
        }
    }

    if (num_workers_ == 0) {
        return PlacementResult::NoWorkers;
    }

    bool is_pinned = false;
    uint32_t worker_id = choose_worker(item.actor, is_pinned);
    if (is_pinned && workers_paused) {
        std::lock_guard<std::mutex> lock(pinned_mutex_);
        pinned_ready_[worker_id].push_back(item);
        return PlacementResult::EnqueuedShared;
    }

    enqueue_shared(item, priority, worker_id);
    return PlacementResult::EnqueuedShared;
}

bool WorkPlacementScheduler::pop_edf(uint32_t worker_id, WorkItem& out) {
    auto& worker = workers_[worker_id];
    if (worker.edf_queue.empty()) {
        return false;
    }
    int64_t deadline = 0;
    if (!worker.edf_queue.peek(deadline)) {
        return false;
    }
    return worker.edf_queue.pop(out);
}

bool WorkPlacementScheduler::pop_local(uint32_t worker_id, WorkItem& out) {
    auto& worker = workers_[worker_id];

    // Drain the shared-input stack (lock-free, pushed by non-owner threads).
    // Reverse from LIFO stack order to approximate FIFO before pushing into
    // the owner's deque, where push_bottom is safe.
    SharedInputNode* head =
        worker.shared_input.exchange(nullptr, std::memory_order_acquire);
    if (head) {
        // Reverse the singly-linked list.
        SharedInputNode* prev = nullptr;
        SharedInputNode* curr = head;
        while (curr) {
            SharedInputNode* next = curr->next.load(std::memory_order_relaxed);
            curr->next.store(prev, std::memory_order_relaxed);
            prev = curr;
            curr = next;
        }
        // Push reversed (FIFO) items into the owner's deque.
        while (prev) {
            SharedInputNode* next = prev->next.load(std::memory_order_relaxed);
            uint8_t prio = prev->priority;
            if (prio < num_priorities_) {
                worker.queues[prio].push_bottom(prev->item);
            }
            delete prev;
            prev = next;
        }
    }

    if (pop_edf(worker_id, out)) {
        return true;
    }
    for (uint32_t p = 0; p < num_priorities_; ++p) {
        if (worker.queues[p].pop_bottom(out)) {
            return true;
        }
    }
    return false;
}

void WorkPlacementScheduler::emit_steal_metric(const WorkItem& item,
                                               uint32_t from_worker) {
    if (metrics_ring_buffer_) {
        metrics::MetricEvent evt{};
        evt.actor_id = item.actor;
        evt.event_type = metrics::MetricEventType::kSchedulerSteal;
        evt.value_hi = from_worker;
        metrics_ring_buffer_->try_push(evt);
    }
}

bool WorkPlacementScheduler::try_steal(uint32_t thief_worker_id, WorkItem& out) {
    for (uint32_t attempt = 0; attempt < num_workers_; ++attempt) {
        uint32_t victim_idx = a2ws_.get_victim(attempt % num_workers_);
        auto& victim = workers_[victim_idx];

        if (victim.edf_queue.pop(out)) {
            a2ws_.record_steal(attempt % num_workers_, victim_idx);
            emit_steal_metric(out, victim_idx);
            HPACTOR_LOG_DEBUG(
                log::LogCategory::kScheduler, out.actor,
                static_cast<uint32_t>(log::LogEventId::kSchedulerSteal),
                "work stolen",
                log::field("from_worker", static_cast<uint64_t>(victim_idx)),
                log::field("to_worker", static_cast<uint64_t>(thief_worker_id)));
            return true;
        }

        for (uint32_t p = 0; p < num_priorities_; ++p) {
            if (victim.queues[p].steal_top(out)) {
                a2ws_.record_steal(attempt % num_workers_, victim_idx);
                emit_steal_metric(out, victim_idx);
                HPACTOR_LOG_DEBUG(
                    log::LogCategory::kScheduler, out.actor,
                    static_cast<uint32_t>(log::LogEventId::kSchedulerSteal),
                    "work stolen",
                    log::field("from_worker", static_cast<uint64_t>(victim_idx)),
                    log::field("to_worker", static_cast<uint64_t>(thief_worker_id)));
                return true;
            }
        }

        a2ws_.record_attempt(attempt % num_workers_, victim_idx, false);
    }
    return false;
}

bool WorkPlacementScheduler::pop_any_for_test(WorkItem& out) {
    for (uint32_t w = 0; w < num_workers_; ++w) {
        // Drain shared input for this worker first.
        auto& worker = workers_[w];
        SharedInputNode* head =
            worker.shared_input.exchange(nullptr, std::memory_order_acquire);
        if (head) {
            SharedInputNode* prev = nullptr;
            SharedInputNode* curr = head;
            while (curr) {
                SharedInputNode* next = curr->next.load(std::memory_order_relaxed);
                curr->next.store(prev, std::memory_order_relaxed);
                prev = curr;
                curr = next;
            }
            while (prev) {
                SharedInputNode* next = prev->next.load(std::memory_order_relaxed);
                uint8_t prio = prev->priority;
                if (prio < num_priorities_) {
                    worker.queues[prio].push_bottom(prev->item);
                }
                delete prev;
                prev = next;
            }
        }
        if (pop_edf(w, out)) {
            return true;
        }
        for (uint32_t p = 0; p < num_priorities_; ++p) {
            if (workers_[w].queues[p].steal_top(out)) {
                return true;
            }
        }
    }
    return false;
}

bool WorkPlacementScheduler::pop_one_on_worker_for_test(uint32_t worker_id,
                                                        WorkItem& out) {
    if (worker_id >= num_workers_) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(pinned_mutex_);
        if (!pinned_ready_[worker_id].empty()) {
            out = pinned_ready_[worker_id].front();
            pinned_ready_[worker_id].pop_front();
            return true;
        }
    }
    // Drain shared input so tests see work even when workers are paused.
    auto& worker = workers_[worker_id];
    SharedInputNode* head =
        worker.shared_input.exchange(nullptr, std::memory_order_acquire);
    if (head) {
        SharedInputNode* prev = nullptr;
        SharedInputNode* curr = head;
        while (curr) {
            SharedInputNode* next = curr->next.load(std::memory_order_relaxed);
            curr->next.store(prev, std::memory_order_relaxed);
            prev = curr;
            curr = next;
        }
        while (prev) {
            SharedInputNode* next = prev->next.load(std::memory_order_relaxed);
            uint8_t prio = prev->priority;
            if (prio < num_priorities_) {
                worker.queues[prio].push_bottom(prev->item);
            }
            delete prev;
            prev = next;
        }
    }
    if (pop_edf(worker_id, out)) {
        return true;
    }
    for (uint32_t p = 0; p < num_priorities_; ++p) {
        if (workers_[worker_id].queues[p].steal_top(out)) {
            return true;
        }
    }
    return false;
}

bool WorkPlacementScheduler::take_pinned_for_test(ActorId actor, WorkItem& out,
                                                  uint32_t& worker_id) {
    std::lock_guard<std::mutex> lock(pinned_mutex_);
    auto it = pinned_actors_.find(actor);
    if (it == pinned_actors_.end() || num_workers_ == 0) {
        return false;
    }
    worker_id = it->second % num_workers_;
    auto& queue = pinned_ready_[worker_id];
    for (auto iter = queue.begin(); iter != queue.end(); ++iter) {
        if (iter->actor == actor) {
            out = *iter;
            queue.erase(iter);
            return true;
        }
    }
    return false;
}

void WorkPlacementScheduler::pin_actor_to_worker(ActorId actor, uint32_t worker_id) {
    std::lock_guard<std::mutex> lock(pinned_mutex_);
    pinned_actors_[actor] = worker_id;
}

void WorkPlacementScheduler::unpin_actor(ActorId actor) {
    std::lock_guard<std::mutex> lock(pinned_mutex_);
    pinned_actors_.erase(actor);
}

void WorkPlacementScheduler::flush_pinned_to_shared() {
    std::lock_guard<std::mutex> lock(pinned_mutex_);
    for (uint32_t w = 0; w < num_workers_; ++w) {
        for (auto& item : pinned_ready_[w]) {
            workers_[w].queues[0].push_bottom(item);
        }
        pinned_ready_[w].clear();
    }
}

void WorkPlacementScheduler::register_dedicated_thread(ActorId actor,
                                                       int cpu_affinity) {
    std::lock_guard<std::mutex> lock(dedicated_->dedicated_mutex_);
    dedicated_->dedicated_thread_actors_.insert(actor);
    if (cpu_affinity >= 0) {
        dedicated_->dedicated_thread_affinity_[actor] = cpu_affinity;
    }
}

void WorkPlacementScheduler::register_dedicated_pool(ActorId actor,
                                                     uint32_t pool_size) {
    std::lock_guard<std::mutex> lock(dedicated_->dedicated_mutex_);
    auto& pool = dedicated_->dedicated_pools_[pool_size];
    if (!pool) {
        pool = std::make_unique<DedicatedThreadPool>(pool_size);
        pool->start();
    }
    dedicated_->actor_pool_map_[actor] = pool_size;
}

void WorkPlacementScheduler::unregister_dedicated(ActorId actor) {
    std::lock_guard<std::mutex> lock(dedicated_->dedicated_mutex_);
    dedicated_->dedicated_thread_actors_.erase(actor);
    dedicated_->dedicated_thread_affinity_.erase(actor);
    dedicated_->actor_pool_map_.erase(actor);
}

} // namespace hpactor::sched
