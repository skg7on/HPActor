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
#include <hpactor/log/log_field.hpp>
#include <hpactor/log/logger.hpp>
#include <hpactor/mailbox/mailbox_policy.hpp>
#include <hpactor/sched/dedicated_thread_pool.hpp>
#include <hpactor/sched/scheduler.hpp>

#include <chrono>
#include <variant>

#if HPACTOR_SUPPORT_COROUTINES
#    include <hpactor/sched/coroutine_task.hpp>
#endif

namespace hpactor::sched {

// DedicatedStorage: holds per-actor dedicated execution state.
// Defined in .cpp to allow DedicatedThreadPool to remain incomplete in the
// header (libc++ noexcept containers require complete types).
struct HybridScheduler::DedicatedStorage {
    // Dedicated thread tracking (thread lifecycle managed by DaemonActor)
    std::unordered_set<ActorId> dedicated_thread_actors_;
    std::unordered_map<ActorId, int> dedicated_thread_affinity_;
    std::mutex dedicated_mutex_;

    // Dedicated thread pools (pool_size -> pool)
    std::unordered_map<uint32_t, std::unique_ptr<DedicatedThreadPool>> dedicated_pools_;
    std::unordered_map<ActorId, uint32_t> actor_pool_map_; // actor -> pool_size
};

// Thread-local pointer to the current worker executing on this thread
thread_local uint32_t tl_current_worker_id = UINT32_MAX;

HybridScheduler::HybridScheduler(ActorSystem& system, uint32_t num_workers,
                                 uint32_t num_priorities,
                                 TimerBackend timer_backend, bool start_paused)
    : system_(system), num_workers_(num_workers), num_priorities_(num_priorities),
      workers_(num_workers), a2ws_(num_workers), workers_paused_(start_paused),
      dedicated_(std::make_unique<DedicatedStorage>()) {
    switch (timer_backend) {
        case TimerBackend::TimingWheel:
            timer_backend_.emplace<TimingWheel>(1'000'000, 4);
            break;
        case TimerBackend::CalendarQueue:
            timer_backend_.emplace<CalendarQueue>();
            break;
    }
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
            std::visit([&](auto& backend) { backend.advance(now); }, timer_backend_);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
}

HybridScheduler::~HybridScheduler() {
    stop();
}

void HybridScheduler::stop() {
    running_.store(false, std::memory_order_release);
    // Wake any workers parked in wait_if_paused so they see running_ == false
    // and exit their loop.
    resume_workers();
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

    WorkItem item{actor, deadline_ns, 0};

    DedicatedThreadPool* dedicated_pool = nullptr;
    {
        std::lock_guard<std::mutex> lock(dedicated_->dedicated_mutex_);

        if (dedicated_->dedicated_thread_actors_.find(actor) !=
            dedicated_->dedicated_thread_actors_.end()) {
            return;
        }

        auto actor_pool = dedicated_->actor_pool_map_.find(actor);
        if (actor_pool != dedicated_->actor_pool_map_.end()) {
            auto pool = dedicated_->dedicated_pools_.find(actor_pool->second);
            if (pool != dedicated_->dedicated_pools_.end()) {
                dedicated_pool = pool->second.get();
            }
        }
    }

    if (dedicated_pool != nullptr) {
        dedicated_pool->enqueue(actor, [this, item]() { execute_actor(item); });
        return;
    }

    // Cooperative path: gate on actor state to prevent double-enqueue.
    // If the actor is already executing (Running) or enqueued (Ready),
    // a second WorkItem would be redundant.  CAS Idle→Ready atomically.
    auto actor_ptr = system_.get_actor(actor);
    if (actor_ptr && actor_ptr->is_event_based_actor()) {
        auto* eb = static_cast<EventBasedActor*>(actor_ptr.get());
        auto& state = eb->actor_state();
        uint32_t current = state.get();
        if (current == ActorState::kReady || current == ActorState::kRunning)
            return;
        if (current == ActorState::kTerminated)
            return;
        if (!state.cas(current, ActorState::kReady))
            return; // another thread won the race
    }

    // Round-robin across workers for fair initial placement.
    // The atomic counter avoids the stale hint issue where get_victim always
    // returns the same value because record_attempt is only called on steals.
    if (num_workers_ == 0)
        return;
    static std::atomic<uint32_t> rr_counter{0};
    uint32_t victim = rr_counter.fetch_add(1, std::memory_order_relaxed);

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
            if (metrics_ring_buffer_) [[unlikely]] {
                metrics::MetricEvent evt{};
                evt.actor_id = out.actor;
                evt.event_type = metrics::MetricEventType::kSchedulerSteal;
                evt.value_hi = victim_idx;
                metrics_ring_buffer_->try_push(evt);
            }
            HPACTOR_LOG_DEBUG(
                log::LogCategory::kScheduler, out.actor,
                static_cast<uint32_t>(log::LogEventId::kSchedulerSteal),
                "work stolen",
                log::field("from_worker", static_cast<uint64_t>(victim_idx)),
                log::field("to_worker",
                           static_cast<uint64_t>(tl_current_worker_id)));
            return true;
        }

        // Try each priority level from highest to lowest
        for (uint32_t p = 0; p < num_priorities_; ++p) {
            if (victim.queues[p].steal_top(out)) {
                a2ws_.record_steal(attempt % num_workers_, victim_idx);
                if (metrics_ring_buffer_) [[unlikely]] {
                    metrics::MetricEvent evt{};
                    evt.actor_id = out.actor;
                    evt.event_type = metrics::MetricEventType::kSchedulerSteal;
                    evt.value_hi = victim_idx;
                    metrics_ring_buffer_->try_push(evt);
                }
                HPACTOR_LOG_DEBUG(
                    log::LogCategory::kScheduler, out.actor,
                    static_cast<uint32_t>(log::LogEventId::kSchedulerSteal),
                    "work stolen",
                    log::field("from_worker", static_cast<uint64_t>(victim_idx)),
                    log::field("to_worker",
                               static_cast<uint64_t>(tl_current_worker_id)));
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
        // Dequeue-time deadline check: drop expired messages before
        // they reach the actor handler.
        uint64_t now_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
        if (mailbox::is_expired(msg.deadline_ns(), now_ns)) {
            if (metrics_ring_buffer_) [[unlikely]] {
                metrics::MetricEvent evt{};
                evt.timestamp_ns = now_ns;
                evt.actor_id = actor;
                evt.event_type = metrics::MetricEventType::kDeliveryExpired;
                evt.code = static_cast<uint8_t>(FailureReason::Expired);
                evt.value_hi = 1;
                metrics_ring_buffer_->try_push(evt);
            }
            // Loop to check for more messages
            process_actor(actor);
            return;
        }
        actor_ptr->receive(msg);
    }
}

void HybridScheduler::execute_actor(const WorkItem& item) {
    auto actor_ptr = system_.get_actor(item.actor);
    if (!actor_ptr || !actor_ptr->is_event_based_actor()) {
        return;
    }

    if (metrics_ring_buffer_) [[unlikely]] {
        metrics::MetricEvent evt{};
        evt.actor_id = item.actor;
        evt.event_type = metrics::MetricEventType::kSchedulerDispatch;
        evt.value_hi = 0; // worker_id filled by the steal event
        metrics_ring_buffer_->try_push(evt);
    }

    HPACTOR_LOG_DEBUG(
        log::LogCategory::kScheduler, item.actor,
        static_cast<uint32_t>(log::LogEventId::kSchedulerDispatch),
        "actor dispatched",
        log::field("worker_id", static_cast<uint64_t>(tl_current_worker_id)));

    auto* actor = static_cast<EventBasedActor*>(actor_ptr.get());

    // Set thread-local current actor ID so SlabAllocated::operator new and
    // mem::current_actor_id() can attribute allocations to the correct actor.
    mem::set_current_actor_id(item.actor);

#if HPACTOR_SUPPORT_COROUTINES
    if (system_.use_coroutines()) {
        // C++20 coroutine path (runtime opt-in via Config::use_coroutines)
        // Lazily start the coroutine on first pickup
        actor->ensure_coroutine_started();

        auto& coroutine = actor->get_actor_coroutine();
        if (!coroutine)
            return;

        auto& promise = coroutine.task().handle().promise();

        // First transition: kIdle/kIOWaiting → kReady (if needed)
        // This handles the case where actor is picked up after suspending
        // on mailbox (kIdle) or on timer/IO (kIOWaiting).
        if (promise.actor_state->is_idle() || promise.actor_state->is_io_waiting()) {
            promise.actor_state->set(ActorState::kReady);
        }

        // Transition: Ready → Running
        // If not in Ready state (already Running/Terminated), skip
        uint32_t expected = ActorState::kReady;
        if (!promise.actor_state->cas(expected, ActorState::kRunning)) {
            if (promise.actor_state->is_terminated()) {
                actor->set_exit_reason(errors::actor_down);
                actor->on_exit();
            }
            // Already running or terminated by another path — skip
            return;
        }

        // Resume the coroutine
        coroutine.resume();

        // Post-resume: coroutine suspended (Idle/IOWaiting) or terminated.
        // Note: cannot access promise after resume() returns if coroutine
        // terminated — the promise is destroyed with the coroutine frame. Use
        // coroutine.done() which checks internal handle state (not the
        // promise).
        if (coroutine.done()) {
            actor->on_exit();
        }
        // If idle or IOWaiting, the actor will be re-woken by:
        // - MailboxAwaiter edge-trigger (MPSCActorMailbox::enqueue →
        // notify_ready)
        // - TimerAwaiter callback (EventLoop → notify_ready)
        // Nothing to do here for suspended actors
        return;
    }
#endif // HPACTOR_SUPPORT_COROUTINES

    // Behavior-based scheduling — state-aware CAS dispatch.
    // Uses the same ActorState machine as the coroutine path:
    // Idle -> Ready -> Running -> Idle (if empty) / Ready (if more work).
    auto& actor_state = actor->actor_state();

    // First transition: kIdle → kReady (if actor is idle on first pickup)
    if (actor_state.is_idle()) {
        actor_state.set(ActorState::kReady);
    }

    // Transition: Ready → Running
    uint32_t expected = ActorState::kReady;
    if (!actor_state.cas(expected, ActorState::kRunning)) {
        if (actor_state.is_terminated()) {
            actor->set_exit_reason(errors::actor_down);
            actor->on_exit();
        }
        return;
    }

    auto mailbox = system_.get_mailbox(item.actor);
    if (!mailbox) {
        actor_state.set(ActorState::kIdle);
        return;
    }

    TypedMessage msg;
    if (mailbox->try_pop(msg)) {
        // Dequeue-time deadline check: drop expired messages before
        // they reach the actor handler.
        uint64_t now_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
        if (mailbox::is_expired(msg.deadline_ns(), now_ns)) {
            if (metrics_ring_buffer_) [[unlikely]] {
                metrics::MetricEvent evt{};
                evt.timestamp_ns = now_ns;
                evt.actor_id = item.actor;
                evt.event_type = metrics::MetricEventType::kDeliveryExpired;
                evt.code = static_cast<uint8_t>(FailureReason::Expired);
                evt.value_hi = 1;
                metrics_ring_buffer_->try_push(evt);
            }
        } else {
            actor->receive(msg);
        }
    }

    if (!mailbox->empty()) {
        // More messages waiting — re-enqueue directly.
        // We set kReady and push to a worker queue, bypassing the state
        // gate in notify_ready() (which would skip kReady actors).
        actor_state.set(ActorState::kReady);
        static std::atomic<uint32_t> rr{0};
        uint32_t v = rr.fetch_add(1, std::memory_order_relaxed);
        workers_[v % num_workers_].queues[0].push_bottom(item);
    } else {
        actor_state.set(ActorState::kIdle);
        // Double-check: a message may have arrived between the empty check
        // and setting Idle.  Push directly to a worker queue, bypassing the
        // state gate in notify_ready() which would skip kReady actors.
        if (!mailbox->empty()) {
            expected = ActorState::kIdle;
            if (actor_state.cas(expected, ActorState::kReady)) {
                static std::atomic<uint32_t> rr2{0};
                uint32_t v2 = rr2.fetch_add(1, std::memory_order_relaxed);
                workers_[v2 % num_workers_].queues[0].push_bottom(item);
            }
        }
    }
}

void HybridScheduler::wait_if_paused(uint32_t worker_id) {
    (void)worker_id;
    if (!workers_paused_.load(std::memory_order_acquire)) {
        return;
    }
    parked_worker_count_.fetch_add(1, std::memory_order_relaxed);
    std::unique_lock<std::mutex> lock(worker_control_mutex_);
    worker_control_cv_.wait(lock, [this] {
        return !workers_paused_.load(std::memory_order_acquire) ||
               !running_.load(std::memory_order_acquire);
    });
    parked_worker_count_.fetch_sub(1, std::memory_order_relaxed);
}

void HybridScheduler::mark_dispatch_begin() noexcept {
    active_worker_dispatches_.fetch_add(1, std::memory_order_release);
}

void HybridScheduler::mark_dispatch_end() noexcept {
    active_worker_dispatches_.fetch_sub(1, std::memory_order_release);
}

void HybridScheduler::worker_loop(uint32_t worker_id) {
    tl_current_worker_id = worker_id; // set thread-local

    HPACTOR_LOG_DEBUG(log::LogCategory::kScheduler, ActorId{0}, 0, "worker started",
                      log::field("worker_id", static_cast<uint64_t>(worker_id)));

    while (running_.load(std::memory_order_acquire)) {
        wait_if_paused(worker_id);

        WorkItem item;

        // Try local pop first (owner operation - wait-free)
        if (pop_local(item, worker_id)) {
            mark_dispatch_begin();
            execute_actor(item);
            mark_dispatch_end();
            continue;
        }

        // Check EDF queue for deadline-ordered work
        if (pop_edf(item, worker_id)) {
            mark_dispatch_begin();
            execute_actor(item);
            mark_dispatch_end();
            continue;
        }

        // Local empty - try stealing (lock-free but may fail)
        if (try_steal(item)) {
            mark_dispatch_begin();
            execute_actor(item);
            mark_dispatch_end();
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

uint64_t
HybridScheduler::schedule_timer(int64_t delay_ns, timer_callback callback) {
    return std::visit(
        [&](auto& backend) {
            return backend.schedule(delay_ns, std::move(callback));
        },
        timer_backend_);
}

void HybridScheduler::advance_time(int64_t now_ns) {
    std::visit([&](auto& backend) { backend.advance(now_ns); }, timer_backend_);
}

TimerHandle HybridScheduler::schedule_after(timer_callback cb, int64_t delay_ns) {
    auto id =
        std::visit([&](auto& backend) { return backend.schedule(delay_ns, cb); },
                   timer_backend_);
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
                std::visit(
                    [&](auto& backend) {
                        static_cast<void>(backend.schedule(*interval, recurring));
                    },
                    timer_backend_);
            }
        }
    };

    auto id = std::visit(
        [&](auto& backend) { return backend.schedule(*interval, recurring); },
        timer_backend_);
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
    auto it = recurring_cancellations_.find(handle.value());
    if (it != recurring_cancellations_.end()) {
        it->second->store(true, std::memory_order_release);
        recurring_cancellations_.erase(it);
    }
    std::visit([&](auto& backend) { backend.cancel(handle.value()); },
               timer_backend_);
}

void HybridScheduler::register_dedicated_thread(ActorId actor, int cpu_affinity) {
    std::lock_guard<std::mutex> lock(dedicated_->dedicated_mutex_);
    dedicated_->dedicated_thread_actors_.insert(actor);
    if (cpu_affinity >= 0) {
        dedicated_->dedicated_thread_affinity_[actor] = cpu_affinity;
    }
}

void HybridScheduler::register_dedicated_pool(ActorId actor, uint32_t pool_size) {
    std::lock_guard<std::mutex> lock(dedicated_->dedicated_mutex_);
    auto& pool = dedicated_->dedicated_pools_[pool_size];
    if (!pool) {
        pool = std::make_unique<DedicatedThreadPool>(pool_size);
        pool->start();
    }
    dedicated_->actor_pool_map_[actor] = pool_size;
}

void HybridScheduler::pause_workers() noexcept {
    workers_paused_.store(true, std::memory_order_release);
    // Wait until no worker is inside actor code.
    while (active_worker_dispatches_.load(std::memory_order_acquire) > 0) {
        std::this_thread::yield();
    }
}

void HybridScheduler::resume_workers() noexcept {
    workers_paused_.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(worker_control_mutex_);
    }
    worker_control_cv_.notify_all();
}

bool HybridScheduler::workers_paused() const noexcept {
    return workers_paused_.load(std::memory_order_acquire);
}

bool HybridScheduler::pop_any_ready(WorkItem& out) {
    // Scan all workers, stealing from each. We must use steal_top() rather
    // than pop_bottom() because the caller (e.g., run_one_ready from test
    // thread) is not the owning worker of any queue.
    for (uint32_t w = 0; w < num_workers_; ++w) {
        auto& worker = workers_[w];
        // Check EDF first
        if (pop_edf(out, w)) {
            return true;
        }
        // Check priority queues, highest to lowest
        for (uint32_t p = 0; p < num_priorities_; ++p) {
            if (worker.queues[p].steal_top(out)) {
                return true;
            }
        }
    }
    return false;
}

bool HybridScheduler::run_one_ready() {
    if (!workers_paused_.load(std::memory_order_acquire)) {
        return false;
    }
    WorkItem item;
    if (!pop_any_ready(item)) {
        return false;
    }
    // Temporarily set thread-local for metrics/logging attribution.
    uint32_t saved_id = tl_current_worker_id;
    tl_current_worker_id = UINT32_MAX;
    execute_actor(item);
    tl_current_worker_id = saved_id;
    return true;
}

SchedulerDrainResult HybridScheduler::drain_ready(size_t max_items) {
    SchedulerDrainResult result;
    for (size_t i = 0; i < max_items; ++i) {
        if (!run_one_ready()) {
            result.idle = true;
            return result;
        }
        ++result.executed;
    }
    result.idle = false;
    return result;
}

void HybridScheduler::unregister_dedicated(ActorId actor) {
    std::lock_guard<std::mutex> lock(dedicated_->dedicated_mutex_);
    dedicated_->dedicated_thread_actors_.erase(actor);
    dedicated_->dedicated_thread_affinity_.erase(actor);
    dedicated_->actor_pool_map_.erase(actor);
}

} // namespace hpactor::sched
