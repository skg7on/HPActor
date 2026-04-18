// include/hpactor/sched/coroutine_task.hpp
#pragma once

#include <hpactor/actor/actor_state.hpp>
#include <hpactor/types/types.hpp>

#include <atomic>
#include <coroutine>
#include <cstdint>
#include <thread>

namespace hpactor::sched {

// Forward declarations
class ActorCoroutine;
class WorkerThread;
class CoroutineTask;

// CoroutinePromise: promise_type for actor coroutines
// Controls lifecycle: initial_suspend → Running → (Suspend | Terminate)
struct CoroutinePromise {
    using handle_type = std::coroutine_handle<CoroutinePromise>;

    ActorId actor_id;
    ActorState state;
    WorkerThread* owner{nullptr};

    // Mailbox integration
    void* mailbox{nullptr};  // MPSCMailbox<MessageNode>*
    std::atomic<bool> mailbox_was_empty{true};

    // Continuation for chained awaiters
    std::coroutine_handle<> continuation;

    CoroutinePromise() = default;
    ~CoroutinePromise() = default;

    // Start suspended — scheduler decides when to resume
    std::suspend_always initial_suspend() noexcept { return {}; }

    // Called on co_return
    void return_void() noexcept {
        state.set(ActorState::kTerminated);
    }

    // Called on unhandled exception
    void unhandled_exception() noexcept {
        state.set(ActorState::kTerminated);
        // Store exception info for error reporting (future)
    }

    CoroutineTask get_return_object();

    // State access
    void set_running() { state.set(ActorState::kRunning); }
    void set_idle() { state.set(ActorState::kIdle); }
    void set_ready() { state.set(ActorState::kReady); }
    void set_io_waiting() { state.set(ActorState::kIOWaiting); }
    void set_terminated() { state.set(ActorState::kTerminated); }

    bool is_idle() const { return state.is_idle(); }
    bool is_running() const { return state.is_running(); }
    bool is_terminated() const { return state.is_terminated(); }

    // Called by MPSCActorMailbox when a message arrives while actor is idle.
    // If actor is suspended (waiting in MailboxAwaiter), resume it.
    void notify_mailbox_nonempty() {
        if (continuation && !continuation.done()) {
            continuation.resume();
        }
    }
};

// CoroutineTask: return type of actor coroutines
// Wraps std::coroutine_handle<CoroutinePromise> and manages actor lifecycle
class CoroutineTask {
public:
    using handle_type = std::coroutine_handle<CoroutinePromise>;

    CoroutineTask() noexcept : handle_(nullptr) {}
    explicit CoroutineTask(handle_type handle) noexcept : handle_(handle) {}

    CoroutineTask(CoroutineTask&& other) noexcept : handle_(other.handle_) {
        other.handle_ = nullptr;
    }

    CoroutineTask(const CoroutineTask&) = delete;
    CoroutineTask& operator=(const CoroutineTask&) = delete;

    CoroutineTask& operator=(CoroutineTask&& other) noexcept {
        if (this != &other) {
            if (handle_) handle_.destroy();
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }

    ~CoroutineTask() {
        if (handle_) handle_.destroy();
    }

    explicit operator bool() const noexcept { return handle_ != nullptr; }

    handle_type handle() const noexcept { return handle_; }

    // Resume the coroutine
    void resume() {
        if (handle_ && !handle_.done()) {
            handle_.resume();
        }
    }

    // Check if done
    bool done() const { return !handle_ || handle_.done(); }

private:
    handle_type handle_;
};

// Out-of-line definition for get_return_object
inline CoroutineTask CoroutinePromise::get_return_object() {
    return CoroutineTask{handle_type::from_promise(*this)};
}

} // namespace hpactor::sched
