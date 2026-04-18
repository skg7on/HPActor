// include/hpactor/sched/yield_awaiter.hpp
#pragma once

#include <hpactor/sched/scheduler.hpp>
#include <hpactor/sched/coroutine_task.hpp>
#include <hpactor/types/types.hpp>

#include <coroutine>
#include <cstdint>

namespace hpactor::sched {

// YieldAwaiter: co_await scheduler.yield(actor_id, priority)
// Voluntarily suspends and immediately re-enqueues the actor at the same priority.
// Used for cooperative multitasking after processing a message.
//
// Note: Currently uses notify_ready() as IScheduler::yield() is added in Task 3.2.
class YieldAwaiter {
public:
    YieldAwaiter(IScheduler* scheduler, ActorId actor_id, uint8_t priority = 0) noexcept
        : scheduler_(scheduler), actor_id_(actor_id), priority_(priority) {}

    bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> continuation) noexcept {
        continuation_ = continuation;
        // Re-enqueue the actor at same priority, then return (suspend)
        // The worker loop will pick up this actor again via notify_ready
        scheduler_->yield(actor_id_, priority_);
    }

    void await_resume() noexcept {}

private:
    IScheduler* scheduler_;
    ActorId actor_id_;
    uint8_t priority_;
    std::coroutine_handle<> continuation_;
};

// SchedulerYield: convenience helper used inside an actor's act() method
// Extracts actor_id from the coroutine's promise.
class SchedulerYield {
public:
    explicit SchedulerYield(IScheduler* scheduler, uint8_t priority = 0) noexcept
        : scheduler_(scheduler), priority_(priority) {}

    bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> continuation) noexcept {
        continuation_ = continuation;
        auto& promise = std::coroutine_handle<CoroutinePromise>::from_address(
                            continuation.address()).promise();
        scheduler_->yield(promise.actor_id, priority_);
    }

    void await_resume() noexcept {}

private:
    IScheduler* scheduler_;
    uint8_t priority_;
    std::coroutine_handle<> continuation_;
};

} // namespace hpactor::sched
