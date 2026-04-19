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

    // Final suspend — keep coroutine frame alive for restart.
    // Actor runtime calls resume() again to re-enter act().
    std::suspend_always final_suspend() noexcept { return {}; }

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

} // namespace hpactor::sched

// Specialize std::coroutine_traits so CoroutineTask can be used as a
// coroutine return type. The compiler looks for coroutine_traits<ReturnType>
// to find promise_type.
template<>
struct std::coroutine_traits<hpactor::sched::CoroutineTask> {
    using promise_type = hpactor::sched::CoroutinePromise;
};

// Specialize for member function calls (EventBasedActor&)
// This allows act() to be a coroutine member function
template<typename T>
struct std::coroutine_traits<hpactor::sched::CoroutineTask, T&> {
    using promise_type = hpactor::sched::CoroutinePromise;
};

template<typename T>
struct std::coroutine_traits<hpactor::sched::CoroutineTask, const T&> {
    using promise_type = hpactor::sched::CoroutinePromise;
};

namespace hpactor::sched {

// Out-of-line definition for get_return_object
inline CoroutineTask CoroutinePromise::get_return_object() {
    return CoroutineTask{handle_type::from_promise(*this)};
}

} // namespace hpactor::sched
