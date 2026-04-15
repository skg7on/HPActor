// include/hpactor/sched/coroutine_awaiters.hpp
#pragma once

#include <hpactor/sched/coroutine_task.hpp>

#include <atomic>
#include <coroutine>
#include <cstdint>

namespace hpactor::sched {

// MailboxAwaiter: awaitable for co_await actor.receive()
// Suspends when mailbox is empty, resumes when message arrives
class MailboxAwaiter {
public:
    explicit MailboxAwaiter(CoroutinePromise& promise) noexcept
        : promise_(promise) {}

    // Return true if already has message (don't suspend)
    bool await_ready() const noexcept {
        return !promise_.mailbox_was_empty.load(std::memory_order_acquire);
    }

    // Called when suspending
    bool await_suspend(std::coroutine_handle<> continuation) noexcept {
        // Transition: Running → Idle
        uint32_t expected = ActorState::kRunning;
        if (promise_.state.cas(expected, ActorState::kIdle)) {
            promise_.continuation = continuation;
            return true;  // successfully suspended
        }
        // State was not Running — actor may have already terminated
        return false;  // don't suspend
    }

    // Called when resuming (message arrived)
    void await_resume() noexcept {
        // State should already be Ready or Running
    }

private:
    CoroutinePromise& promise_;
};

// TimerAwaiter: awaitable for co_await scheduler.schedule_after(delay)
class TimerAwaiter {
public:
    TimerAwaiter(int64_t delay_ns, uint64_t& timer_id_out,
                 std::coroutine_handle<>& cont_out) noexcept
        : delay_ns_(delay_ns), timer_id_out_(timer_id_out), continuation_(cont_out) {}

    bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> continuation) noexcept {
        continuation_ = continuation;
        // Caller will schedule timer and set timer_id_out_
        return true;
    }

    void await_resume() noexcept {
        // Timer fired — continuation was already resumed
    }

private:
    int64_t delay_ns_;
    uint64_t& timer_id_out_;
    std::coroutine_handle<>& continuation_;
};

// BlockingMailboxAwaiter: for blocking receive with stackful coroutines
class BlockingMailboxAwaiter {
public:
    BlockingMailboxAwaiter(CoroutinePromise& promise,
                           void* frame_pool,
                           std::coroutine_handle<> continuation) noexcept
        : promise_(promise), frame_pool_(frame_pool), continuation_(continuation) {}

    bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> continuation) noexcept {
        promise_.continuation = continuation;
        return true;
    }

    void await_resume() noexcept {
        // Returns the message
    }

private:
    CoroutinePromise& promise_;
    void* frame_pool_;
    std::coroutine_handle<> continuation_;
};

} // namespace hpactor::sched