#include <hpactor/net/event_loop.hpp>

#include <cstring>
#include <cstdlib>

// Platform-specific includes
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__)
#include <sys/event.h>
#include <unistd.h>
#elif defined(__linux__)
#include <sys/epoll.h>
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

#endif

} // namespace net
} // namespace hpactor
