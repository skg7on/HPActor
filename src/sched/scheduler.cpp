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

#include <hpactor/sched/scheduler.hpp>
#include <hpactor/core/actor_system.hpp>

namespace hpactor::sched {

HybridScheduler::HybridScheduler(ActorSystem& system, uint32_t num_workers, uint32_t num_priorities)
    : system_(system), num_workers_(num_workers), num_priorities_(num_priorities),
      workers_(num_workers), a2ws_(num_workers) {
    for (uint32_t i = 0; i < num_workers; ++i) {
        workers_[i].queues = std::make_unique<ChaselevDeque<WorkItem>[]>(num_priorities);
        workers_[i].index = i;
    }
}

void HybridScheduler::start() {
    if (running_.load(std::memory_order_acquire)) {
        return;
    }
    running_.store(true, std::memory_order_release);
}

HybridScheduler::~HybridScheduler() {
    stop();
}

void HybridScheduler::stop() {
    running_.store(false, std::memory_order_release);
}

void HybridScheduler::enqueue(ActorId actor, uint8_t priority) {
    if (!running_.load(std::memory_order_acquire)) {
        return;
    }

    // Simple round-robin assignment to workers
    // TODO: Could use affinity or load estimation for better placement
    uint32_t victim = a2ws_.get_victim(0);  // Use A2WS for initial placement
    (void)victim;  // TODO: Use affinity hint
    WorkItem item{actor, INT64_MAX, 0};
    // Push to highest priority queue (priority is inverted: 0 = highest)
    workers_[0].queues[priority].push_bottom(item);
}

void HybridScheduler::enqueue_deadline(ActorId actor, uint8_t priority, int64_t deadline_ns) {
    if (!running_.load(std::memory_order_acquire)) {
        return;
    }

    uint32_t victim = a2ws_.get_victim(0);  // Use A2WS for initial placement
    (void)victim;  // TODO: Use affinity hint
    (void)priority;  // Priority is handled by the EDF queue ordering
    WorkItem item{actor, deadline_ns, 0};

    // If deadline is urgent (soon), push to EDF queue for priority processing
    // Otherwise push to regular priority queue
    // For now, push to the victim's EDF queue
    workers_[victim % num_workers_].edf_queue.push(deadline_ns, item);
}

bool HybridScheduler::try_steal(WorkItem& out) {
    // Use A2WS for adaptive victim selection
    for (uint32_t attempt = 0; attempt < num_workers_; ++attempt) {
        // Get next victim from A2WS
        uint32_t victim_idx = a2ws_.get_victim(attempt % num_workers_);

        auto& victim = workers_[victim_idx];

        // Try EDF queue first (deadline-ordered work has highest urgency)
        if (victim.edf_queue.pop(out)) {
            a2ws_.record_steal(attempt % num_workers_, victim_idx);
            return true;
        }

        // Try each priority level from highest to lowest
        for (uint32_t p = 0; p < num_priorities_; ++p) {
            if (victim.queues[p].steal_top(out)) {
                a2ws_.record_steal(attempt % num_workers_, victim_idx);
                return true;
            }
        }

        // Record failed attempt
        a2ws_.record_attempt(attempt % num_workers_, victim_idx, false);
    }
    return false;
}

bool HybridScheduler::pop_local(WorkItem& out, uint32_t worker_id) {
    auto& worker = workers_[worker_id];

    // Check EDF queue first for deadline-ordered work
    if (pop_edf(out, worker_id)) {
        return true;
    }

    // Check priority queues from highest to lowest
    for (uint32_t p = 0; p < num_priorities_; ++p) {
        if (worker.queues[p].pop_bottom(out)) {
            return true;
        }
    }
    return false;
}

bool HybridScheduler::pop_edf(WorkItem& out, uint32_t worker_id) {
    auto& worker = workers_[worker_id];

    // Check EDF queue
    if (worker.edf_queue.empty()) {
        return false;
    }

    // Check if earliest deadline is urgent (within next ~10ms)
    // For now, just return the earliest deadline item
    int64_t deadline;
    if (worker.edf_queue.peek(deadline)) {
        // In a real implementation, we'd check if deadline < now + threshold
        // For simplicity, just process EDF items when they exist
        return worker.edf_queue.pop(out);
    }
    return false;
}

void HybridScheduler::process_actor(ActorId actor) {
    auto actor_ptr = system_.get_actor(actor);
    if (!actor_ptr) {
        return;
    }

    auto mailbox = system_.get_mailbox(actor);
    if (!mailbox) {
        return;
    }

    Message<MessageVariant> msg;
    if (mailbox->try_pop(msg)) {
        actor_ptr->receive(msg.move_payload());
    }
}

void HybridScheduler::worker_loop(uint32_t worker_id) {
    while (running_.load(std::memory_order_acquire)) {
        WorkItem item;

        // Try local pop first (owner operation - wait-free)
        if (pop_local(item, worker_id)) {
            process_actor(item.actor);
            continue;
        }

        // Local empty - try stealing (lock-free but may fail)
        if (try_steal(item)) {
            process_actor(item.actor);
            continue;
        }

        // No work available - backoff
        // TODO: Implement proper backoff (exponential, yield, etc.)
    }
}

} // namespace hpactor::sched