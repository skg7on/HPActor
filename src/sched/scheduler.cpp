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

#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/hpactor_config.hpp>
#include <hpactor/sched/scheduler.hpp>

#if HPACTOR_SUPPORT_COROUTINES
#    include <hpactor/sched/coroutine_task.hpp>
#endif

namespace hpactor::sched {

// Thread-local pointer to the current worker executing on this thread
thread_local uint32_t tl_current_worker_id = UINT32_MAX;

HybridScheduler::HybridScheduler(ActorSystem& system, uint32_t num_workers,
                                 uint32_t num_priorities)
    : system_(system), num_workers_(num_workers), num_priorities_(num_priorities),
      workers_(num_workers), a2ws_(num_workers), timer_wheel_(1'000'000, 4) {
    for (uint32_t i = 0; i < num_workers; ++i) {
        workers_[i].queues =
            std::make_unique<ChaselevDeque<WorkItem>[]>(num_priorities);
        workers_[i].index = i;
    }
}

void HybridScheduler::start() {
    if (running_.load(std::memory_order_acquire)) {
        return;
    }
    running_.store(true, std::memory_order_release);

    worker_threads_.reserve(workers_.size());
    for (size_t i = 0; i < workers_.size(); ++i) {
        worker_threads_.emplace_back(
            [this, i] { worker_loop(static_cast<uint32_t>(i)); });
    }

    // Start timer advancement thread
    timer_thread_ = std::thread([this] {
        while (running_.load(std::memory_order_acquire)) {
            auto now = std::chrono::steady_clock::now().time_since_epoch().count();
            timer_wheel_.advance(now);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
}

HybridScheduler::~HybridScheduler() {
    stop();
}

void HybridScheduler::stop() {
    running_.store(false, std::memory_order_release);
    for (auto& t : worker_threads_) {
        if (t.joinable())
            t.join();
    }
    worker_threads_.clear();

    // Stop and join timer thread
    if (timer_thread_.joinable()) {
        timer_thread_.join();
    }
}

void HybridScheduler::notify_ready(ActorId actor, uint8_t priority,
                                   int64_t deadline_ns) {
    if (!running_.load(std::memory_order_acquire)) {
        return;
    }

    // Round-robin across workers for fair initial placement.
    // The atomic counter avoids the stale hint issue where get_victim always
    // returns the same value because record_attempt is only called on steals.
    static std::atomic<uint32_t> rr_counter{0};
    uint32_t victim = rr_counter.fetch_add(1, std::memory_order_relaxed);
    WorkItem item{actor, deadline_ns, 0};

    // If deadline is INT64_MAX, use priority queue; otherwise use EDF queue
    if (deadline_ns == INT64_MAX) {
        // Push to priority queue using A2WS-selected victim
        workers_[victim % num_workers_].queues[priority].push_bottom(item);
    } else {
        // Push to EDF queue for deadline-ordered processing
        workers_[victim % num_workers_].edf_queue.push(deadline_ns, item);
    }
}

void HybridScheduler::notify_idle(ActorId actor) {
    // Remove actor from EDF tracking if it was scheduled there
    // For now, this is a stub - full implementation would need EDF cancellation
    (void)actor;
}

void HybridScheduler::yield(ActorId actor, uint8_t priority) {
    notify_ready(actor, priority, INT64_MAX);
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

    TypedMessage msg;
    if (mailbox->try_pop(msg)) {
        actor_ptr->receive(msg);
    }
}

void HybridScheduler::execute_actor(const WorkItem& item) {
    auto actor_ptr = system_.get_actor(item.actor);
    if (!actor_ptr || !actor_ptr->is_event_based_actor()) {
        return;
    }
    auto* actor = static_cast<EventBasedActor*>(actor_ptr.get());

#if HPACTOR_SUPPORT_COROUTINES
    if (system_.use_coroutines()) {
        // C++20 coroutine path (runtime opt-in via Config::use_coroutines)
        // Lazily start the coroutine on first pickup
        actor->ensure_coroutine_started();

        auto& coroutine = actor->get_actor_coroutine();
        if (!coroutine)
            return;

        auto& promise = coroutine.task().handle().promise();

        // First transition: kIdle → kReady (if needed)
        // This handles the case where actor is picked up after suspending
        if (promise.state.is_idle()) {
            promise.state.set(ActorState::kReady);
        }

        // Transition: Ready → Running
        // If not in Ready state (already Running/Terminated), skip
        uint32_t expected = ActorState::kReady;
        if (!promise.state.cas(expected, ActorState::kRunning)) {
            if (promise.state.is_terminated()) {
                actor->on_exit();
            }
            // Already running, idle, or IOWaiting — skip
            return;
        }

        // Resume the coroutine
        coroutine.resume();

        // Post-resume: coroutine suspended (Idle/IOWaiting) or terminated.
        // Note: cannot access promise after resume() returns if coroutine
        // terminated — the promise is destroyed with the coroutine frame. Use
        // coroutine.done() which checks internal handle state (not the promise).
        if (coroutine.done()) {
            actor->on_exit();
        }
        // If idle or IOWaiting, the actor will be re-woken by:
        // - MailboxAwaiter edge-trigger (MPSCActorMailbox::enqueue → notify_ready)
        // - TimerAwaiter callback (EventLoop → notify_ready)
        // Nothing to do here for suspended actors
        return;
    }
#endif // HPACTOR_SUPPORT_COROUTINES

    // Behavior-based scheduling (default)
    // Process actor by dispatching messages through Behavior
    // The actor's receive() method calls the current behavior handler

    auto mailbox = system_.get_mailbox(item.actor);
    if (!mailbox) {
        return;
    }

    TypedMessage msg;
    if (mailbox->try_pop(msg)) {
        actor->receive(msg);
    }

    // After processing one message, check if there are more messages waiting.
    // If so, re-enqueue the actor for immediate processing (no yield needed).
    // This prevents message accumulation while still allowing fairness.
    if (!mailbox->empty()) {
        notify_ready(item.actor, 0, INT64_MAX);
    }
}

void HybridScheduler::worker_loop(uint32_t worker_id) {
    tl_current_worker_id = worker_id; // set thread-local

    while (running_.load(std::memory_order_acquire)) {
        WorkItem item;

        // Try local pop first (owner operation - wait-free)
        if (pop_local(item, worker_id)) {
            execute_actor(item);
            continue;
        }

        // Check EDF queue for deadline-ordered work
        if (pop_edf(item, worker_id)) {
            execute_actor(item);
            continue;
        }

        // Local empty - try stealing (lock-free but may fail)
        if (try_steal(item)) {
            execute_actor(item);
            continue;
        }

        // No work available - backoff
        backoff();
    }
}

uint32_t HybridScheduler::current_worker_id() const {
    return tl_current_worker_id;
}

void HybridScheduler::backoff() {
    // Exponential backoff: yield for small counts, sleep for larger
    static thread_local uint32_t count = 0;
    uint32_t c = count++;

    if (c < 4) {
        std::this_thread::yield();
    } else {
        // Sleep for a short interval (exponential, capped)
        uint32_t backoff_us = std::min<uint32_t>(1024u, 10u << (c - 4));
        std::this_thread::sleep_for(std::chrono::microseconds(backoff_us));
    }
}

uint64_t HybridScheduler::schedule_timer(int64_t delay_ns,
                                         TimingWheel::TimerCallback callback) {
    return timer_wheel_.schedule(delay_ns, std::move(callback));
}

void HybridScheduler::advance_time(int64_t now_ns) {
    timer_wheel_.advance(now_ns);
}

TimerHandle HybridScheduler::schedule_after(timer_callback cb, int64_t delay_ns) {
    auto id = timer_wheel_.schedule(delay_ns, [cb = std::move(cb)]() { cb(); });
    return TimerHandle{id};
}

TimerHandle
HybridScheduler::schedule_every(timer_callback cb, int64_t interval_ns) {
    // For recurring timers, we wrap the callback to reschedule itself
    // We need to use a shared_ptr to hold the interval value and the callback
    // to avoid lifecycle issues with the lambda. We also use a cancellation
    // flag to allow the recurring chain to be stopped.
    auto cancelled = std::make_shared<std::atomic<bool>>(false);
    auto interval = std::make_shared<int64_t>(interval_ns);
    auto callback = std::make_shared<timer_callback>(std::move(cb));

    std::function<void()> recurring;
    recurring = [this, cancelled, interval, callback, recurring]() {
        if (cancelled->load(std::memory_order_acquire))
            return;
        if (running_.load(std::memory_order_acquire)) {
            (*callback)();
            if (!cancelled->load(std::memory_order_acquire)) {
                timer_wheel_.schedule(*interval, recurring);
            }
        }
    };

    auto id = timer_wheel_.schedule(*interval, recurring);
    {
        std::lock_guard<std::mutex> lock(cancellation_mutex_);
        recurring_cancellations_[id] = cancelled;
    }
    return TimerHandle{id};
}

void HybridScheduler::cancel_timer(TimerHandle handle) {
    if (!handle.valid())
        return;

    std::lock_guard<std::mutex> lock(cancellation_mutex_);
    auto it = recurring_cancellations_.find(handle.id);
    if (it != recurring_cancellations_.end()) {
        it->second->store(true, std::memory_order_release);
        recurring_cancellations_.erase(it);
    }
    timer_wheel_.cancel(handle.id);
}

} // namespace hpactor::sched
