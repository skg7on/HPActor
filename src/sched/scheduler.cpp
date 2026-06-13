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
#include <hpactor/fault/fault_macros.hpp>
#include <hpactor/hpactor_config.hpp>
#include <hpactor/log/log_field.hpp>
#include <hpactor/log/logger.hpp>
#include <hpactor/mem/memory_config.hpp>
#include <hpactor/msg/enqueue_result.hpp>
#include <hpactor/sched/scheduler.hpp>
#include <hpactor/sched/worker_thread.hpp>

#include <algorithm>
#include <chrono>
#include <variant>

namespace hpactor::sched {

// Thread-local pointer to the current worker executing on this thread
thread_local uint32_t tl_current_worker_id = UINT32_MAX;

HybridScheduler::HybridScheduler(ActorSystem& system, uint32_t num_workers,
                                 uint32_t num_priorities,
                                 TimerBackend timer_backend, bool start_paused)
    : system_(system), ready_gate_(system),
      placement_(num_workers, num_priorities), executor_(system, ready_gate_),
      num_workers_(num_workers), workers_paused_(start_paused) {
    switch (timer_backend) {
        case TimerBackend::TimingWheel:
            timer_backend_.emplace<TimingWheel>(1'000'000, 4);
            break;
        case TimerBackend::CalendarQueue: {
            auto make_storage = [](size_t sz) -> void* {
                return mem::allocate(mem::RegionType::kInternal, sz,
                                     hpactor::ActorId{});
            };
            auto destroy_storage = [](void* p, size_t) { mem::deallocate(p); };
            timer_backend_.emplace<CalendarQueue>(CalendarQueueConfig{},
                                                  make_storage, destroy_storage);
            break;
        }
    }
}

void HybridScheduler::start() {
    if (running_.load(std::memory_order_acquire)) {
        return;
    }
    running_.store(true, std::memory_order_release);

    worker_threads_.reserve(placement_.worker_count());
    for (size_t i = 0; i < placement_.worker_count(); ++i) {
        WorkerThread::Config cfg;
        cfg.worker_index = static_cast<uint32_t>(i);
        cfg.enable_thread_allocator = false; // slab caches are not thread-safe
        auto worker = std::make_unique<WorkerThread>(cfg);
        worker->set_owner(this);
        worker->set_work_processor([this](const WorkItem& item) {
            mark_dispatch_begin();
            execute_actor(item);
            mark_dispatch_end();
        });
        worker->set_pause_handler([this] { wait_if_paused(0); });
        worker->set_fault_controller(&system_.fault_controller());
        worker->start();
        worker_threads_.push_back(std::move(worker));
    }

    // Start timer advancement thread.
    // When workers are paused (test mode), skip timer advancement so
    // timer callbacks don't race with drain_ready() on worker deques.
    timer_thread_ = std::thread([this] {
        while (running_.load(std::memory_order_acquire)) {
            if (!workers_paused_.load(std::memory_order_acquire)) {
                // Advance first so already-due timers fire before we sleep.
                advance_time(
                    std::chrono::steady_clock::now().time_since_epoch().count());
                // Read the cached next deadline (lock-free).
                int64_t next_ns = INT64_MAX;
                std::visit(
                    [&](auto& backend) { next_ns = backend.next_deadline(); },
                    timer_backend_);
                auto now =
                    std::chrono::steady_clock::now().time_since_epoch().count();
                // Sleep until the deadline.  Floor at 1 ms to avoid
                // tight-looping; cap at 100 ms for shutdown responsiveness.
                int64_t sleep_ns = std::max(
                    std::min(next_ns - now, static_cast<int64_t>(100'000'000)),
                    static_cast<int64_t>(1'000'000));
                std::this_thread::sleep_for(std::chrono::nanoseconds(sleep_ns));
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
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

    // Stop the timer thread first: workers may be blocked on
    // TimingWheel::schedule() waiting for the mutex held by advance().
    // Once the timer thread exits, the mutex is released and workers
    // can unblock and join.
    if (timer_thread_.joinable()) {
        timer_thread_.join();
    }

    for (auto& worker : worker_threads_) {
        worker->stop();
    }
    worker_threads_.clear();
}

bool HybridScheduler::try_admit_ready(ActorId actor) noexcept {
    return ready_gate_.try_mark_ready(actor).accepted();
}

bool HybridScheduler::try_mark_yield_ready(ActorId actor) noexcept {
    auto actor_ptr = system_.get_actor(actor);
    if (!actor_ptr || !actor_ptr->is_event_based_actor()) {
        return false;
    }
    auto* eb = static_cast<EventBasedActor*>(actor_ptr.get());
    return ready_gate_.mark_ready_already_admitted(*eb).accepted();
}

void HybridScheduler::enqueue_admitted(const WorkItem& item, uint8_t priority) {
    placement_.enqueue_admitted(item, priority,
                                workers_paused_.load(std::memory_order_acquire),
                                [this](const WorkItem& dedicated_item) {
                                    mark_dispatch_begin();
                                    execute_actor(dedicated_item);
                                    mark_dispatch_end();
                                });
}

void HybridScheduler::notify_ready(ActorId actor, uint8_t priority,
                                   int64_t deadline_ns) {
    if (!running_.load(std::memory_order_acquire)) {
        return;
    }
    FAULT_INJECT("hpactor.scheduler.worker.pause") {
        _fc->stall(hpactor::fault::FaultDomain::kScheduler, /*delay_ticks=*/5);
    }
    FAULT_INJECT("hpactor.scheduler.notify_ready.drop") {
        return;
    }

    WorkItem item{actor, deadline_ns, 0};

    if (!try_admit_ready(actor)) {
        return;
    }

    enqueue_admitted(item, priority);
}

void HybridScheduler::notify_idle(ActorId actor) {
    // Remove actor from EDF tracking if it was scheduled there
    // For now, this is a stub - full implementation would need EDF cancellation
    (void)actor;
}

void HybridScheduler::yield(ActorId actor, uint8_t priority) {
    if (!running_.load(std::memory_order_acquire)) {
        return;
    }
    if (!try_mark_yield_ready(actor)) {
        return;
    }
    enqueue_admitted(WorkItem{actor, INT64_MAX, 0}, priority);
}

bool HybridScheduler::try_steal(WorkItem& out) {
    FAULT_INJECT("hpactor.scheduler.try_steal.fail") {
        return false;
    }
    return placement_.try_steal(tl_current_worker_id, out);
}

bool HybridScheduler::pop_local(WorkItem& out, uint32_t worker_id) {
    FAULT_INJECT("hpactor.scheduler.pop_local.fail") {
        return false;
    }
    return placement_.pop_local(worker_id, out);
}

bool HybridScheduler::pop_edf(WorkItem& out, uint32_t worker_id) {
    return placement_.pop_edf(worker_id, out);
}

void HybridScheduler::execute_actor(const WorkItem& item) {
    FAULT_INJECT("hpactor.scheduler.execute_actor.dispatch_skip") {
        return;
    }
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

    ActorExecutionContext execution_context{
        tl_current_worker_id,
        metrics_ring_buffer_,
        logger_,
        workers_paused_.load(std::memory_order_relaxed),
    };

    auto result =
        executor_.run(*actor, item, execution_context, system_.use_coroutines());
    if (result.disposition == ActorRunDisposition::RequeueReady) {
        // Carry a sequence counter so BehaviorActorRunner::run() can cap
        // consecutive RequeueReady cycles and force a yield.  Wrap at 128
        // so the counter resets naturally after a forced yield.
        uint64_t next_seq = item.sequence + 1;
        if (next_seq > 128)
            next_seq = 0;
        enqueue_admitted(WorkItem{item.actor, result.deadline_ns, next_seq},
                         result.priority);
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

uint32_t HybridScheduler::current_worker_id() const {
    return tl_current_worker_id;
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

    auto recurring = std::make_shared<std::function<void()>>();
    *recurring = [this, cancelled, interval, callback, recurring]() {
        if (cancelled->load(std::memory_order_acquire))
            return;
        if (running_.load(std::memory_order_acquire)) {
            (*callback)();
            if (!cancelled->load(std::memory_order_acquire)) {
                std::visit(
                    [&](auto& backend) {
                        static_cast<void>(backend.schedule(*interval, *recurring));
                    },
                    timer_backend_);
            }
        }
    };

    auto id = std::visit(
        [&](auto& backend) { return backend.schedule(*interval, *recurring); },
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
    placement_.register_dedicated_thread(actor, cpu_affinity);
}

void HybridScheduler::register_dedicated_pool(ActorId actor, uint32_t pool_size) {
    placement_.register_dedicated_pool(actor, pool_size);
}

void HybridScheduler::pause_workers() noexcept {
    workers_paused_.store(true, std::memory_order_release);
    // Wait until no worker is inside actor code.
    while (active_worker_dispatches_.load(std::memory_order_acquire) > 0) {
        std::this_thread::yield();
    }
}

void HybridScheduler::resume_workers() noexcept {
    placement_.flush_pinned_to_shared();
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
    return placement_.pop_any_for_test(out);
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

void HybridScheduler::pin_actor_to_worker(ActorId actor, uint32_t worker_id) {
    placement_.pin_actor_to_worker(actor, worker_id);
}

void HybridScheduler::unpin_actor(ActorId actor) {
    placement_.unpin_actor(actor);
}

bool HybridScheduler::run_actor(ActorId actor) {
    if (!workers_paused_.load(std::memory_order_acquire)) {
        return false;
    }
    WorkItem item;
    uint32_t worker_id;
    if (!placement_.take_pinned_for_test(actor, item, worker_id)) {
        return false;
    }
    uint32_t saved_id = tl_current_worker_id;
    tl_current_worker_id = worker_id;
    execute_actor(item);
    tl_current_worker_id = saved_id;
    return true;
}

bool HybridScheduler::run_one_on_worker(uint32_t worker_id) {
    if (!workers_paused_.load(std::memory_order_acquire)) {
        return false;
    }
    WorkItem item;
    if (!placement_.pop_one_on_worker_for_test(worker_id, item)) {
        return false;
    }
    uint32_t saved_id = tl_current_worker_id;
    tl_current_worker_id = worker_id;
    execute_actor(item);
    tl_current_worker_id = saved_id;
    return true;
}

void HybridScheduler::unregister_dedicated(ActorId actor) {
    placement_.unregister_dedicated(actor);
}

std::vector<WorkerSnapshot> HybridScheduler::worker_snapshots() const {
    std::vector<WorkerSnapshot> result;
    result.reserve(worker_threads_.size());
    for (size_t i = 0; i < worker_threads_.size(); ++i) {
        WorkerSnapshot ws;
        ws.worker_index = static_cast<uint16_t>(i);
        ws.is_idle = !worker_threads_[i]->is_running() ||
                     worker_threads_[i]->depth() == 0;
        ws.steals_attempted = worker_threads_[i]->donation_count();
        ws.steals_successful = 0;
        ws.actors_executed = 0;
        ws.work_found = worker_threads_[i]->diag_work_found();
        ws.idle_iters = worker_threads_[i]->diag_idle_iters();
        ws.cv_escalations = worker_threads_[i]->diag_cv_escalations();
        ws.cv_notify_wakes = worker_threads_[i]->diag_cv_notify_wakes();
        ws.cv_timeout_wakes = worker_threads_[i]->diag_cv_timeout_wakes();
        ws.thread_id = worker_threads_[i]->thread_id();
        result.push_back(ws);
    }
    return result;
}

int64_t HybridScheduler::edf_next_deadline() noexcept {
    int64_t earliest = INT64_MAX;
    int64_t deadline = 0;
    for (auto& ws : placement_.workers()) {
        if (ws.edf_queue.peek(deadline) && deadline < earliest) {
            earliest = deadline;
        }
    }
    return earliest;
}

} // namespace hpactor::sched
