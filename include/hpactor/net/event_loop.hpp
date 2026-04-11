#pragma once

#include <hpactor/types.hpp>

#include <functional>
#include <memory>
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

private:
    int kqueue_fd_;  // kqueue fd on macOS/BSD, epoll fd on Linux
};

} // namespace net
} // namespace hpactor
