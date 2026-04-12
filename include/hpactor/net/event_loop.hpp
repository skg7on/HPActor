#pragma once

#include <hpactor/types.hpp>

#include <atomic>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

namespace hpactor {

namespace net {

// -----------------------------------------------------------------------------
// EventLoop - platform-specific I/O event notification
// -----------------------------------------------------------------------------
// Provides edge-triggered I/O event notification using:
//   - epoll on Linux
//   - kqueue on macOS/BSD
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
    bool update_fd(int fd, Event events);
    bool remove_fd(int fd);

    // Wait for events (blocking with timeout)
    // Returns number of events triggered, 0 on timeout, -1 on error
    int wait(int timeout_ms);

    // Get triggered events for an fd
    bool has_event(int fd, Event event) const;

    // Get the underlying system fd (for socket operations)
    int system_fd() const { return kqueue_fd_; }

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

private:
    int kqueue_fd_;  // kqueue fd on macOS/BSD, epoll fd on Linux

    std::unordered_map<uint64_t, timer_callback> timer_callbacks_;
    std::atomic<uint64_t> next_timer_handle_{1};
};

} // namespace net
} // namespace hpactor
