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

#include <hpactor/net/reactor_backend.hpp>

#include <atomic>
#include <functional>
#include <memory>
#include <unordered_map>
#include <unordered_set>

namespace hpactor {

class ActorSystem;

namespace net {

// -----------------------------------------------------------------------------
// EventLoop - async I/O backend wrapper
// -----------------------------------------------------------------------------
// Provides a unified interface over platform-specific async I/O backends:
//   - io_uring on Linux (preferred), epoll fallback
//   - libdispatch (GCD) on macOS (preferred), kqueue fallback
//
// Usage:
//   EventLoop loop;  // Creates backend but doesn't start
//   loop.run();       // Explicitly start processing
//   // ... event loop runs ...
//   loop.stop();      // Stop when done
// -----------------------------------------------------------------------------
class EventLoop {
  public:
    EventLoop();
    ~EventLoop();

    // Non-copyable, non-movable
    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;
    EventLoop(EventLoop&&) = delete;
    EventLoop& operator=(EventLoop&&) = delete;

    // Start the backend and begin processing events
    // Call this explicitly after construction
    bool run();

    // Stop the backend
    void stop();

    // Check if the backend is running
    bool is_running() const {
        return running_.load();
    }

    // Get the backend name for debugging
    const char* backend_name() const;

    // File descriptor registration
    enum class Event : uint32_t {
        Read = 1,
        Write = 2,
        EdgeTriggered = 4,
    };

    // Add or update a file descriptor interest
    bool add_fd(int fd, Event events);

    // Update an existing fd registration
    bool update_fd(int fd, Event events);

    // Remove an fd registration
    bool remove_fd(int fd);

    // Read handler callback type - called when data is received
    using read_callback = std::function<void(const bytes&)>;

    // Set a read handler for an FD. When data arrives, it's delivered to this
    // callback. The EventLoop will automatically issue async_recv for the FD
    // when it becomes readable.
    void set_read_handler(int fd, read_callback handler);

    // Remove read handler for an FD
    void clear_read_handler(int fd);

    // Returns true if the backend supports calling read handlers directly
    // from wait(). Reactor backends return true, proactor return false.
    bool supports_read_handler() const;

    // Wait for events (blocking with timeout)
    // Returns number of events triggered, 0 on timeout, -1 on error
    int wait(int timeout_ms);

    // Get triggered events for an fd
    bool has_event(int fd, Event event) const;

    // Timer callback type
    using timer_callback = std::function<void()>;

    // Schedule a one-shot timer to fire after delay_ms milliseconds
    // Returns a timer handle that can be used to cancel the timer
    uint64_t run_after(timer_callback callback, int delay_ms);

    // Schedule a repeating timer to fire every interval_ms milliseconds
    // Returns a timer handle that can be used to cancel the timer
    uint64_t run_every(timer_callback callback, int interval_ms);

    // Cancel a scheduled timer
    void cancel_timer(uint64_t timer_handle);

    // Process completions from the backend (called by wait loop)
    void process_completions();

    // Enqueue a completion to be delivered to an actor
    // Called by proactor backend via its deliver_completion
    void enqueue_completion(OpCompletion completion);

    // Set the ActorSystem for delivering completions
    void set_actor_system(ActorSystem* actor_system);

    // Set callback for test verification (test-only API)
    using completion_callback = std::function<void(OpCompletion)>;
    void set_completion_callback(completion_callback cb) {
        completion_callback_ = std::move(cb);
    }

    // Get the underlying backend for direct async operations
    IReactorBackend* backend() {
        return backend_.get();
    }

  private:
    // Deliver a timer completion to the stored callback
    void deliver_timer_completion(OpCompletion completion);

    std::unique_ptr<IReactorBackend> backend_;
    std::atomic<bool> running_{false};
    const char* backend_name_ = "unknown";

    // Map timer handles to callbacks (for bridging backend completions to
    // callbacks)
    std::unordered_map<uint64_t, timer_callback> timer_callbacks_;
    std::atomic<uint64_t> next_timer_handle_{1};

    // Map backend timer handles to our timer handles
    std::unordered_map<uint64_t, uint64_t> backend_handle_to_handle_;

    // Set of timer handles that are repeating (run_every)
    std::unordered_set<uint64_t> repeating_timers_;

    // For has_event tracking
    std::unordered_map<int, Event> fd_events_;

    // Optional callback for test verification (disabled in production)
    completion_callback completion_callback_;

    ActorSystem* actor_system_ = nullptr;
};

} // namespace net
} // namespace hpactor
