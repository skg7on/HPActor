#pragma once

#include <hpactor/net/async_io_backend.hpp>

#include <atomic>
#include <functional>
#include <memory>
#include <unordered_map>

namespace hpactor {

class ActorSystem;

namespace net {

// -----------------------------------------------------------------------------
// EventLoop - async I/O backend wrapper
// -----------------------------------------------------------------------------
// Provides a unified interface over platform-specific async I/O backends:
//   - io_uring on Linux
//   - libdispatch (GCD) on macOS
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
    // Called by AsyncIoBackend via its deliver_completion
    void enqueue_completion(OpCompletion completion);

    // Set the ActorSystem for delivering completions
    void set_actor_system(ActorSystem* actor_system);

    // Get the underlying backend for direct async operations
    AsyncIoBackend* backend() { return backend_.get(); }

private:
    // Deliver a timer completion to the stored callback
    void deliver_timer_completion(OpCompletion completion);

    std::unique_ptr<AsyncIoBackend> backend_;

    // Map timer handles to callbacks (for bridging backend completions to callbacks)
    std::unordered_map<uint64_t, timer_callback> timer_callbacks_;
    std::atomic<uint64_t> next_timer_handle_{1};

    // Map backend timer handles to our timer handles
    std::unordered_map<uint64_t, uint64_t> backend_handle_to_handle_;

    // For has_event tracking
    std::unordered_map<int, Event> fd_events_;

    ActorSystem* actor_system_ = nullptr;
};

} // namespace net
} // namespace hpactor
