#include <hpactor/net/event_loop.hpp>

#include <cstring>
#include <cstdlib>

// Platform-specific includes
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__)
#include <sys/event.h>
#include <unistd.h>
#elif defined(__linux__)
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <unistd.h>
#else
#error "Unsupported platform"
#endif

namespace hpactor {

namespace net {

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__)
// -----------------------------------------------------------------------------
// kqueue implementation (macOS, BSD)
// -----------------------------------------------------------------------------

EventLoop::EventLoop() : kqueue_fd_(kqueue()) {
    // kqueue_fd_ is initialized in member initializer
}

EventLoop::~EventLoop() {
    if (kqueue_fd_ >= 0) {
        ::close(kqueue_fd_);
    }
}

bool EventLoop::add_fd(int fd, Event events) {
    struct kevent ke;
    uint16_t flags = EV_ADD | EV_ENABLE;

    if (int(events) & int(Event::EdgeTriggered)) {
        flags |= EV_CLEAR;
    }

    EV_SET(&ke, fd, EVFILT_READ, flags, 0, 0, nullptr);
    if (kevent(kqueue_fd_, &ke, 1, nullptr, 0, nullptr) != 0) {
        return false;
    }

    if (int(events) & int(Event::Write)) {
        EV_SET(&ke, fd, EVFILT_WRITE, flags, 0, 0, nullptr);
        return kevent(kqueue_fd_, &ke, 1, nullptr, 0, nullptr) == 0;
    }
    return true;
}

bool EventLoop::update_fd(int fd, Event events) {
    return add_fd(fd, events);  // kqueue auto-updates
}

bool EventLoop::remove_fd(int fd) {
    struct kevent ke;
    EV_SET(&ke, fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
    kevent(kqueue_fd_, &ke, 1, nullptr, 0, nullptr);

    EV_SET(&ke, fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
    kevent(kqueue_fd_, &ke, 1, nullptr, 0, nullptr);
    return true;
}

int EventLoop::wait(int timeout_ms) {
    struct timespec ts;
    ts.tv_sec = timeout_ms / 1000;
    ts.tv_nsec = static_cast<long>(timeout_ms % 1000) * 1000000;

    std::vector<struct kevent> events(64);
    return kevent(kqueue_fd_, nullptr, 0, events.data(),
                  static_cast<int>(events.size()), &ts);
}

bool EventLoop::has_event(int /*fd*/, Event /*event*/) const {
    // For kqueue, triggered events are returned from wait()
    // Caller should iterate wait() results
    return true;
}

uint64_t EventLoop::run_after(timer_callback callback, int delay_ms) {
    uint64_t handle = next_timer_handle_++;
    timer_callbacks_[handle] = std::move(callback);

    int pipe_fds[2];
    if (pipe(pipe_fds) < 0) {
        timer_callbacks_.erase(handle);
        return 0;
    }

    struct kevent ke;
    // EVFILT_TIMER: data field contains timeout in milliseconds
    // Use NOTE_ABSOLUTE for absolute time, otherwise relative
    EV_SET(&ke, pipe_fds[0], EVFILT_TIMER, EV_ADD | EV_ENABLE | EV_ONESHOT, 0, delay_ms, nullptr);
    if (kevent(kqueue_fd_, &ke, 1, nullptr, 0, nullptr) != 0) {
        timer_callbacks_.erase(handle);
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return 0;
    }
    // Note: pipe_fds[1] is kept for potential future use
    (void)pipe_fds;  // Avoid unused warning
    return handle;
}

uint64_t EventLoop::run_every(timer_callback callback, int interval_ms) {
    uint64_t handle = next_timer_handle_++;
    timer_callbacks_[handle] = std::move(callback);

    int pipe_fds[2];
    if (pipe(pipe_fds) < 0) {
        timer_callbacks_.erase(handle);
        return 0;
    }

    struct kevent ke;
    EV_SET(&ke, pipe_fds[0], EVFILT_TIMER, EV_ADD | EV_ENABLE, 0, interval_ms, nullptr);
    if (kevent(kqueue_fd_, &ke, 1, nullptr, 0, nullptr) != 0) {
        timer_callbacks_.erase(handle);
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return 0;
    }
    (void)pipe_fds;
    return handle;
}

void EventLoop::cancel_timer(uint64_t timer_handle) {
    timer_callbacks_.erase(timer_handle);
}

#elif defined(__linux__)
// -----------------------------------------------------------------------------
// epoll implementation (Linux)
// -----------------------------------------------------------------------------

EventLoop::EventLoop() : kqueue_fd_(epoll_create1(0)) {
    // kqueue_fd_ is initialized in member initializer
}

EventLoop::~EventLoop() {
    if (kqueue_fd_ >= 0) {
        ::close(kqueue_fd_);
    }
}

bool EventLoop::add_fd(int fd, Event events) {
    struct epoll_event ev;
    std::memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN | EPOLLET;
    if (int(events) & int(Event::Write)) {
        ev.events |= EPOLLOUT;
    }
    ev.data.fd = fd;
    return epoll_ctl(kqueue_fd_, EPOLL_CTL_ADD, fd, &ev) == 0;
}

bool EventLoop::update_fd(int fd, Event events) {
    struct epoll_event ev;
    std::memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN | EPOLLET;
    if (int(events) & int(Event::Write)) {
        ev.events |= EPOLLOUT;
    }
    ev.data.fd = fd;
    return epoll_ctl(kqueue_fd_, EPOLL_CTL_MOD, fd, &ev) == 0;
}

bool EventLoop::remove_fd(int fd) {
    epoll_ctl(kqueue_fd_, EPOLL_CTL_DEL, fd, nullptr);
    return true;
}

int EventLoop::wait(int timeout_ms) {
    std::vector<struct epoll_event> events(64);
    return epoll_wait(kqueue_fd_, events.data(),
                      static_cast<int>(events.size()), timeout_ms);
}

bool EventLoop::has_event(int /*fd*/, Event /*event*/) const {
    // For epoll, triggered events are returned from wait()
    // Caller should iterate wait() results
    return true;
}

uint64_t EventLoop::run_after(timer_callback callback, int delay_ms) {
    uint64_t handle = next_timer_handle_++;
    timer_callbacks_[handle] = std::move(callback);

    int timer_fd = timerfd_create(CLOCK_MONOTONIC, 0);
    if (timer_fd < 0) {
        timer_callbacks_.erase(handle);
        return 0;
    }

    struct itimerspec ts;
    ts.it_value.tv_sec = delay_ms / 1000;
    ts.it_value.tv_nsec = (delay_ms % 1000) * 1000000;
    ts.it_interval.tv_sec = 0;
    ts.it_interval.tv_nsec = 0;
    timerfd_settime(timer_fd, 0, &ts, nullptr);

    struct epoll_event ev;
    std::memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = timer_fd;
    if (epoll_ctl(kqueue_fd_, EPOLL_CTL_ADD, timer_fd, &ev) != 0) {
        timer_callbacks_.erase(handle);
        close(timer_fd);
        return 0;
    }
    return handle;
}

uint64_t EventLoop::run_every(timer_callback callback, int interval_ms) {
    uint64_t handle = next_timer_handle_++;
    timer_callbacks_[handle] = std::move(callback);

    int timer_fd = timerfd_create(CLOCK_MONOTONIC, 0);
    if (timer_fd < 0) {
        timer_callbacks_.erase(handle);
        return 0;
    }

    struct itimerspec ts;
    ts.it_value.tv_sec = interval_ms / 1000;
    ts.it_value.tv_nsec = (interval_ms % 1000) * 1000000;
    ts.it_interval.tv_sec = interval_ms / 1000;
    ts.it_interval.tv_nsec = (interval_ms % 1000) * 1000000;
    timerfd_settime(timer_fd, 0, &ts, nullptr);

    struct epoll_event ev;
    std::memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = timer_fd;
    if (epoll_ctl(kqueue_fd_, EPOLL_CTL_ADD, timer_fd, &ev) != 0) {
        timer_callbacks_.erase(handle);
        close(timer_fd);
        return 0;
    }
    return handle;
}

void EventLoop::cancel_timer(uint64_t timer_handle) {
    timer_callbacks_.erase(timer_handle);
}

#endif

} // namespace net
} // namespace hpactor
