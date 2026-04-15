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

#include <hpactor/net/epoll_backend.hpp>
#include <hpactor/net/event_loop.hpp>

#if defined(__linux__)
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <cstring>
#include <cstdlib>
#include <ctime>
#endif

namespace hpactor {
namespace net {

#if defined(__linux__)

EpollBackend::EpollBackend() = default;

EpollBackend::~EpollBackend() {
    stop();
}

bool EpollBackend::start() {
    epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd_ < 0) {
        return false;
    }

    // Create timerfd for timer events
    timerfd_ = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (timerfd_ < 0) {
        close(epoll_fd_);
        epoll_fd_ = -1;
        return false;
    }

    // Add timerfd to epoll
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.ptr = nullptr;
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, timerfd_, &ev) < 0) {
        close(timerfd_);
        close(epoll_fd_);
        timerfd_ = -1;
        epoll_fd_ = -1;
        return false;
    }

    running_ = true;
    return true;
}

void EpollBackend::stop() {
    if (!running_) {
        return;
    }
    running_ = false;

    if (timerfd_ >= 0) {
        close(timerfd_);
        timerfd_ = -1;
    }

    if (epoll_fd_ >= 0) {
        close(epoll_fd_);
        epoll_fd_ = -1;
    }

    timers_.clear();
    cancelled_timers_.clear();
    handle_to_timer_index_.clear();
    fd_events_.clear();
}

bool EpollBackend::add_fd(int fd, IoEvent events) {
    uint32_t epoll_events = 0;
    if (int(events) & int(IoEvent::Read)) {
        epoll_events |= EPOLLIN;
    }
    if (int(events) & int(IoEvent::Write)) {
        epoll_events |= EPOLLOUT;
    }

    struct epoll_event ev;
    ev.events = epoll_events | EPOLLET;
    ev.data.fd = fd;

    int op = fd_events_.count(fd) ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
    if (epoll_ctl(epoll_fd_, op, fd, &ev) < 0) {
        return false;
    }

    fd_events_[fd] = epoll_events | EPOLLET;
    return true;
}

bool EpollBackend::update_fd(int fd, IoEvent events) {
    return add_fd(fd, events);  // Same operation
}

bool EpollBackend::remove_fd(int fd) {
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr) < 0) {
        return false;
    }
    fd_events_.erase(fd);
    return true;
}

int EpollBackend::register_buffer(const void* addr, size_t len) {
    (void)addr;
    (void)len;
    // Fixed buffers not supported with epoll
    return -1;
}

bool EpollBackend::unregister_buffer(int buffer_id) {
    (void)buffer_id;
    return false;
}

uint64_t EpollBackend::encode_user_data(int fd, ActorId actor, uint32_t op_type) {
    return (static_cast<uint64_t>(fd) & 0xFFFFFFFFULL) |
           ((actor.value() & 0xFFFFULL) << 32) |
           ((static_cast<uint64_t>(op_type) & 0xFFULL) << 56);
}

void EpollBackend::decode_user_data(uint64_t ud, int& fd, ActorId& actor, uint32_t& op_type) {
    fd = static_cast<int>(ud & 0xFFFFFFFFULL);
    actor = ActorId(static_cast<uint32_t>((ud >> 32) & 0xFFFFULL));
    op_type = static_cast<uint32_t>((ud >> 56) & 0xFFULL);
}

int EpollBackend::process_timers() {
    int triggered = 0;

    // Read expired timerfd events
    uint64_t expirations;
    while (read(timerfd_, &expirations, sizeof(expirations)) > 0) {
        triggered++;
    }

    // Calculate current time
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int64_t now_ms = ts.tv_sec * 1000 + ts.tv_nsec / 1000000;

    // Process expired timers
    for (auto& timer : timers_) {
        if (timer.expires_at_ms <= now_ms) {
            if (cancelled_timers_.count(timer.handle) == 0) {
                OpCompletion completion{
                    .actor = timer.actor,
                    .type = OpType::TimerFired,
                    .fd = -1,
                    .result = 0,
                    .user_data = timer.handle,
                };
                deliver_completion(completion);
                triggered++;

                // Reschedule if repeating
                if (timer.interval_ms > 0) {
                    timer.expires_at_ms = now_ms + timer.interval_ms;
                } else {
                    timer.expires_at_ms = -1;  // Mark for removal
                }
            }
        }
    }

    // Remove expired one-shot timers
    timers_.erase(
        std::remove_if(timers_.begin(), timers_.end(),
                       [](const TimerEntry& t) { return t.expires_at_ms < 0; }),
        timers_.end()
    );

    // Sort remaining timers by expiry
    std::sort(timers_.begin(), timers_.end(),
              [](const TimerEntry& a, const TimerEntry& b) {
                  return a.expires_at_ms < b.expires_at_ms;
              });

    // Update timerfd to next expiry if we have timers
    if (!timers_.empty()) {
        int64_t nextExpiry = timers_.front().expires_at_ms;
        struct itimerspec new_val;
        new_val.it_value.tv_sec = nextExpiry / 1000;
        new_val.it_value.tv_nsec = (nextExpiry % 1000) * 1000000;
        new_val.it_interval.tv_sec = 0;
        new_val.it_interval.tv_nsec = 0;
        timerfd_settime(timerfd_, 0, &new_val, nullptr);
    } else {
        // No timers, disarm
        struct itimerspec new_val;
        new_val.it_value.tv_sec = 0;
        new_val.it_value.tv_nsec = 0;
        new_val.it_interval.tv_sec = 0;
        new_val.it_interval.tv_nsec = 0;
        timerfd_settime(timerfd_, 0, &new_val, nullptr);
    }

    return triggered;
}

uint64_t EpollBackend::run_after(ActorId actor, int delay_ms) {
    std::lock_guard<std::mutex> lock(mutex_);

    uint64_t handle = next_timer_handle_++;

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int64_t expires_at_ms = ts.tv_sec * 1000 + ts.tv_nsec / 1000000 + delay_ms;

    TimerEntry entry;
    entry.expires_at_ms = expires_at_ms;
    entry.actor = actor;
    entry.interval_ms = 0;
    entry.handle = handle;

    timers_.push_back(entry);
    handle_to_timer_index_[handle] = timers_.size() - 1;

    // Sort timers by expiry
    std::sort(timers_.begin(), timers_.end(),
              [](const TimerEntry& a, const TimerEntry& b) {
                  return a.expires_at_ms < b.expires_at_ms;
              });

    // Update timerfd to fire at next timer
    struct itimerspec new_val;
    new_val.it_value.tv_sec = expires_at_ms / 1000;
    new_val.it_value.tv_nsec = (expires_at_ms % 1000) * 1000000;
    new_val.it_interval.tv_sec = 0;
    new_val.it_interval.tv_nsec = 0;
    timerfd_settime(timerfd_, 0, &new_val, nullptr);

    return handle;
}

uint64_t EpollBackend::run_every(ActorId actor, int interval_ms) {
    std::lock_guard<std::mutex> lock(mutex_);

    uint64_t handle = next_timer_handle_++;

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int64_t expires_at_ms = ts.tv_sec * 1000 + ts.tv_nsec / 1000000 + interval_ms;

    TimerEntry entry;
    entry.expires_at_ms = expires_at_ms;
    entry.actor = actor;
    entry.interval_ms = interval_ms;
    entry.handle = handle;

    timers_.push_back(entry);

    // Sort timers by expiry
    std::sort(timers_.begin(), timers_.end(),
              [](const TimerEntry& a, const TimerEntry& b) {
                  return a.expires_at_ms < b.expires_at_ms;
              });

    // Update timerfd to fire at next timer
    struct itimerspec new_val;
    new_val.it_value.tv_sec = expires_at_ms / 1000;
    new_val.it_value.tv_nsec = (expires_at_ms % 1000) * 1000000;
    new_val.it_interval.tv_sec = 0;
    new_val.it_interval.tv_nsec = 0;
    timerfd_settime(timerfd_, 0, &new_val, nullptr);

    return handle;
}

void EpollBackend::cancel_timer(uint64_t handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    cancelled_timers_.insert(handle);
    handle_to_timer_index_.erase(handle);
}

void EpollBackend::async_send(int fd, const iovec* bufs, int buf_count,
                              ActorId actor, uint32_t op_type) {
    // Use non-blocking send in a background context
    // For now, just do synchronous send (blocks) and deliver completion
    // In a full implementation, would use a thread pool
    size_t total_len = 0;
    for (int i = 0; i < buf_count; ++i) {
        total_len += bufs[i].iov_len;
    }

    std::vector<uint8_t> data(total_len);
    size_t offset = 0;
    for (int i = 0; i < buf_count; ++i) {
        std::memcpy(data.data() + offset, bufs[i].iov_base, bufs[i].iov_len);
        offset += bufs[i].iov_len;
    }

    ssize_t n = ::send(fd, data.data(), data.size(), 0);
    int result = (n < 0) ? errno : static_cast<int>(n);

    OpCompletion completion{
        .actor = actor,
        .type = static_cast<OpType>(op_type),
        .fd = fd,
        .result = result,
        .user_data = 0,
    };
    deliver_completion(completion);
}

void EpollBackend::async_recv(int fd, const iovec* bufs, int buf_count,
                              ActorId actor, uint32_t op_type) {
    // Use non-blocking recv
    ssize_t n = ::recvmsg(fd, nullptr, 0);  // Just peek to see if data available
    if (n < 0 && errno == EAGAIN) {
        // Would block - need to wait for fd to be readable
        // For now, return EAGAIN as the result
        n = -EAGAIN;
    }

    // Actually do the recv
    n = ::readv(fd, bufs, buf_count);
    int result = (n < 0) ? errno : static_cast<int>(n);

    OpCompletion completion{
        .actor = actor,
        .type = static_cast<OpType>(op_type),
        .fd = fd,
        .result = result,
        .user_data = 0,
    };
    deliver_completion(completion);
}

void EpollBackend::async_send_fixed(int fd, int buffer_id, size_t offset, size_t len,
                                    ActorId actor, uint32_t op_type) {
    (void)fd;
    (void)buffer_id;
    (void)offset;
    (void)len;
    // Fixed buffers not supported with epoll
    OpCompletion completion{
        .actor = actor,
        .type = static_cast<OpType>(op_type),
        .fd = fd,
        .result = -1,
        .user_data = 0,
    };
    deliver_completion(completion);
}

void EpollBackend::async_recv_fixed(int fd, int buffer_id, size_t offset, size_t len,
                                    ActorId actor, uint32_t op_type) {
    (void)fd;
    (void)buffer_id;
    (void)offset;
    (void)len;
    // Fixed buffers not supported with epoll
    OpCompletion completion{
        .actor = actor,
        .type = static_cast<OpType>(op_type),
        .fd = fd,
        .result = -1,
        .user_data = 0,
    };
    deliver_completion(completion);
}

void EpollBackend::async_accept(int fd, ActorId actor) {
    int client_fd = ::accept(fd, nullptr, nullptr);
    if (client_fd < 0) {
        OpCompletion completion{
            .actor = actor,
            .type = OpType::Accept,
            .fd = -1,
            .result = errno,
            .user_data = 0,
        };
        deliver_completion(completion);
    } else {
        OpCompletion completion{
            .actor = actor,
            .type = OpType::Accept,
            .fd = client_fd,
            .result = client_fd,
            .user_data = 0,
        };
        deliver_completion(completion);
    }
}

void EpollBackend::async_connect(int fd, const sockaddr* addr, socklen_t addrlen,
                                 ActorId actor) {
    int ret = ::connect(fd, addr, addrlen);
    int result;
    if (ret < 0) {
        if (errno == EINPROGRESS) {
            // Connection in progress - wait for fd to be writable
            result = 0;
        } else {
            result = errno;
        }
    } else {
        result = 0;  // Connected immediately
    }

    OpCompletion completion{
        .actor = actor,
        .type = OpType::Connect,
        .fd = fd,
        .result = result,
        .user_data = 0,
    };
    deliver_completion(completion);
}

void EpollBackend::async_recvfrom(int fd, const iovec* bufs, int buf_count,
                                  ActorId actor, uint32_t op_type) {
    struct sockaddr_storage addr;
    socklen_t addrlen = sizeof(addr);

    ssize_t n = ::recvfrom(fd, nullptr, 0, MSG_PEEK, nullptr, nullptr);
    if (n < 0 && errno == EAGAIN) {
        // Would block
        OpCompletion completion{
            .actor = actor,
            .type = static_cast<OpType>(op_type),
            .fd = fd,
            .result = -EAGAIN,
            .user_data = 0,
        };
        deliver_completion(completion);
        return;
    }

    n = ::recvmsg(fd, nullptr, 0);
    int result = (n < 0) ? errno : static_cast<int>(n);

    OpCompletion completion{
        .actor = actor,
        .type = static_cast<OpType>(op_type),
        .fd = fd,
        .result = result,
        .user_data = 0,
    };
    deliver_completion(completion);
}

void EpollBackend::async_sendto(int fd, const iovec* bufs, int buf_count,
                                 const sockaddr* addr, socklen_t addrlen,
                                 ActorId actor, uint32_t op_type) {
    ssize_t n = ::sendto(fd, nullptr, 0, 0, addr, addrlen);
    if (n < 0 && errno == EAGAIN) {
        OpCompletion completion{
            .actor = actor,
            .type = static_cast<OpType>(op_type),
            .fd = fd,
            .result = -EAGAIN,
            .user_data = 0,
        };
        deliver_completion(completion);
        return;
    }

    // Actually send
    size_t total_len = 0;
    for (int i = 0; i < buf_count; ++i) {
        total_len += bufs[i].iov_len;
    }
    std::vector<uint8_t> data(total_len);
    size_t offset = 0;
    for (int i = 0; i < buf_count; ++i) {
        std::memcpy(data.data() + offset, bufs[i].iov_base, bufs[i].iov_len);
        offset += bufs[i].iov_len;
    }

    n = ::sendto(fd, data.data(), data.size(), 0, addr, addrlen);
    int result = (n < 0) ? errno : static_cast<int>(n);

    OpCompletion completion{
        .actor = actor,
        .type = static_cast<OpType>(op_type),
        .fd = fd,
        .result = result,
        .user_data = 0,
    };
    deliver_completion(completion);
}

int EpollBackend::wait(int timeout_ms) {
    struct epoll_event events[16];
    int num_events = epoll_wait(epoll_fd_, events, 16, timeout_ms);

    if (num_events < 0) {
        if (errno == EINTR) {
            return 0;  // Interrupted, not an error
        }
        return -1;
    }

    // Check for timerfd events
    for (int i = 0; i < num_events; ++i) {
        if (events[i].data.fd == timerfd_) {
            process_timers();
        }
    }

    return num_events;
}

void EpollBackend::process_completions() {
    // Completions are delivered immediately in epoll backend
    // This is a no-op for now
}

void EpollBackend::deliver_completion(OpCompletion completion) {
    if (loop_) {
        loop_->enqueue_completion(completion);
    }
}

#else // !defined(__linux__)

// Stub implementations for non-Linux

EpollBackend::EpollBackend() = default;
EpollBackend::~EpollBackend() = default;

bool EpollBackend::start() { return false; }
void EpollBackend::stop() {}

bool EpollBackend::add_fd(int fd, IoEvent events) {
    (void)fd; (void)events;
    return false;
}
bool EpollBackend::update_fd(int fd, IoEvent events) {
    (void)fd; (void)events;
    return false;
}
bool EpollBackend::remove_fd(int fd) {
    (void)fd;
    return false;
}

int EpollBackend::register_buffer(const void* addr, size_t len) {
    (void)addr; (void)len;
    return -1;
}
bool EpollBackend::unregister_buffer(int buffer_id) {
    (void)buffer_id;
    return false;
}

void EpollBackend::async_send(int fd, const iovec* bufs, int buf_count,
                               ActorId actor, uint32_t op_type) {
    (void)fd; (void)bufs; (void)buf_count; (void)actor; (void)op_type;
}
void EpollBackend::async_recv(int fd, const iovec* bufs, int buf_count,
                               ActorId actor, uint32_t op_type) {
    (void)fd; (void)bufs; (void)buf_count; (void)actor; (void)op_type;
}

void EpollBackend::async_send_fixed(int fd, int buffer_id, size_t offset, size_t len,
                                     ActorId actor, uint32_t op_type) {
    (void)fd; (void)buffer_id; (void)offset; (void)len; (void)actor; (void)op_type;
}
void EpollBackend::async_recv_fixed(int fd, int buffer_id, size_t offset, size_t len,
                                     ActorId actor, uint32_t op_type) {
    (void)fd; (void)buffer_id; (void)offset; (void)len; (void)actor; (void)op_type;
}

void EpollBackend::async_accept(int fd, ActorId actor) {
    (void)fd; (void)actor;
}
void EpollBackend::async_connect(int fd, const sockaddr* addr, socklen_t addrlen,
                                  ActorId actor) {
    (void)fd; (void)addr; (void)addrlen; (void)actor;
}

void EpollBackend::async_recvfrom(int fd, const iovec* bufs, int buf_count,
                                   ActorId actor, uint32_t op_type) {
    (void)fd; (void)bufs; (void)buf_count; (void)actor; (void)op_type;
}
void EpollBackend::async_sendto(int fd, const iovec* bufs, int buf_count,
                                 const sockaddr* addr, socklen_t addrlen,
                                 ActorId actor, uint32_t op_type) {
    (void)fd; (void)bufs; (void)buf_count; (void)addr; (void)addrlen; (void)actor; (void)op_type;
}

uint64_t EpollBackend::run_after(ActorId actor, int delay_ms) {
    (void)actor; (void)delay_ms;
    return 0;
}
uint64_t EpollBackend::run_every(ActorId actor, int interval_ms) {
    (void)actor; (void)interval_ms;
    return 0;
}
void EpollBackend::cancel_timer(uint64_t handle) {
    (void)handle;
}

int EpollBackend::wait(int timeout_ms) {
    (void)timeout_ms;
    return -1;
}
void EpollBackend::process_completions() {}

void EpollBackend::deliver_completion(OpCompletion completion) {
    (void)completion;
}

#endif // defined(__linux__)

} // namespace net
} // namespace hpactor
