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

#include <hpactor/net/kqueue_backend.hpp>
#include <hpactor/net/event_loop.hpp>

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
#include <sys/event.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <cstring>
#include <cstdlib>
#endif

namespace hpactor {
namespace net {

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)

KqueueBackend::KqueueBackend() = default;

KqueueBackend::~KqueueBackend() {
    stop();
}

bool KqueueBackend::start() {
    kqueue_fd_ = kqueue();
    if (kqueue_fd_ < 0) {
        return false;
    }

    running_ = true;
    return true;
}

void KqueueBackend::stop() {
    if (!running_) {
        return;
    }
    running_ = false;

    if (kqueue_fd_ >= 0) {
        close(kqueue_fd_);
        kqueue_fd_ = -1;
    }

    timers_.clear();
    cancelled_timers_.clear();
    handle_to_timer_index_.clear();
    fd_events_.clear();
}

bool KqueueBackend::add_fd(int fd, IoEvent events) {
    struct kevent ev[2];
    int nevents = 0;

    // Set non-blocking on fd
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    if (int(events) & int(IoEvent::Read)) {
        EV_SET(&ev[nevents], fd, EVFILT_READ, EV_ADD, 0, 0, nullptr);
        nevents++;
    }
    if (int(events) & int(IoEvent::Write)) {
        EV_SET(&ev[nevents], fd, EVFILT_WRITE, EV_ADD, 0, 0, nullptr);
        nevents++;
    }

    if (nevents > 0) {
        if (kevent(kqueue_fd_, ev, nevents, nullptr, 0, nullptr) < 0) {
            return false;
        }
    }

    fd_events_[fd] = static_cast<uint32_t>(events);
    return true;
}

bool KqueueBackend::update_fd(int fd, IoEvent events) {
    // Remove existing filters
    struct kevent ev[2];
    int nevents = 0;

    if (fd_events_.count(fd)) {
        EV_SET(&ev[nevents], fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
        nevents++;
        EV_SET(&ev[nevents], fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
        nevents++;
        kevent(kqueue_fd_, ev, nevents, nullptr, 0, nullptr);
    }

    // Add new filters
    nevents = 0;
    if (int(events) & int(IoEvent::Read)) {
        EV_SET(&ev[nevents], fd, EVFILT_READ, EV_ADD, 0, 0, nullptr);
        nevents++;
    }
    if (int(events) & int(IoEvent::Write)) {
        EV_SET(&ev[nevents], fd, EVFILT_WRITE, EV_ADD, 0, 0, nullptr);
        nevents++;
    }

    if (nevents > 0) {
        if (kevent(kqueue_fd_, ev, nevents, nullptr, 0, nullptr) < 0) {
            return false;
        }
    }

    fd_events_[fd] = static_cast<uint32_t>(events);
    return true;
}

bool KqueueBackend::remove_fd(int fd) {
    struct kevent ev[2];
    int nevents = 0;

    EV_SET(&ev[nevents], fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
    nevents++;
    EV_SET(&ev[nevents], fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
    nevents++;

    kevent(kqueue_fd_, ev, nevents, nullptr, 0, nullptr);
    fd_events_.erase(fd);
    return true;
}

int KqueueBackend::register_buffer(const void* addr, size_t len) {
    (void)addr;
    (void)len;
    // Fixed buffers not supported with kqueue
    return -1;
}

bool KqueueBackend::unregister_buffer(int buffer_id) {
    (void)buffer_id;
    return false;
}

uint64_t KqueueBackend::encode_user_data(int fd, ActorId actor, uint32_t op_type) {
    return (static_cast<uint64_t>(fd) & 0xFFFFFFFFULL) |
           ((actor.value() & 0xFFFFULL) << 32) |
           ((static_cast<uint64_t>(op_type) & 0xFFULL) << 56);
}

void KqueueBackend::decode_user_data(uint64_t ud, int& fd, ActorId& actor, uint32_t& op_type) {
    fd = static_cast<int>(ud & 0xFFFFFFFFULL);
    actor = ActorId(static_cast<uint32_t>((ud >> 32) & 0xFFFFULL));
    op_type = static_cast<uint32_t>((ud >> 56) & 0xFFULL);
}

uint64_t KqueueBackend::run_after(ActorId actor, int delay_ms) {
    std::lock_guard<std::mutex> lock(mutex_);

    uint64_t handle = next_timer_handle_++;

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int64_t now_ms = ts.tv_sec * 1000 + ts.tv_nsec / 1000000;

    TimerEntry entry;
    entry.expires_at_ms = now_ms + delay_ms;
    entry.actor = actor;
    entry.interval_ms = 0;
    entry.handle = handle;

    timers_.push_back(entry);

    return handle;
}

uint64_t KqueueBackend::run_every(ActorId actor, int interval_ms) {
    std::lock_guard<std::mutex> lock(mutex_);

    uint64_t handle = next_timer_handle_++;

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int64_t now_ms = ts.tv_sec * 1000 + ts.tv_nsec / 1000000;

    TimerEntry entry;
    entry.expires_at_ms = now_ms + interval_ms;
    entry.actor = actor;
    entry.interval_ms = interval_ms;
    entry.handle = handle;

    timers_.push_back(entry);

    return handle;
}

void KqueueBackend::cancel_timer(uint64_t handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    cancelled_timers_.insert(handle);
}

void KqueueBackend::async_send(int fd, const iovec* bufs, int buf_count,
                              ActorId actor, uint32_t op_type) {
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

void KqueueBackend::async_recv(int fd, const iovec* bufs, int buf_count,
                              ActorId actor, uint32_t op_type) {
    ssize_t n = ::readv(fd, bufs, buf_count);
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

void KqueueBackend::async_send_fixed(int fd, int buffer_id, size_t offset, size_t len,
                                    ActorId actor, uint32_t op_type) {
    (void)fd;
    (void)buffer_id;
    (void)offset;
    (void)len;
    // Fixed buffers not supported with kqueue
    OpCompletion completion{
        .actor = actor,
        .type = static_cast<OpType>(op_type),
        .fd = fd,
        .result = -1,
        .user_data = 0,
    };
    deliver_completion(completion);
}

void KqueueBackend::async_recv_fixed(int fd, int buffer_id, size_t offset, size_t len,
                                    ActorId actor, uint32_t op_type) {
    (void)fd;
    (void)buffer_id;
    (void)offset;
    (void)len;
    // Fixed buffers not supported with kqueue
    OpCompletion completion{
        .actor = actor,
        .type = static_cast<OpType>(op_type),
        .fd = fd,
        .result = -1,
        .user_data = 0,
    };
    deliver_completion(completion);
}

void KqueueBackend::async_accept(int fd, ActorId actor) {
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

void KqueueBackend::async_connect(int fd, const sockaddr* addr, socklen_t addrlen,
                                 ActorId actor) {
    int ret = ::connect(fd, addr, addrlen);
    int result;
    if (ret < 0) {
        if (errno == EINPROGRESS) {
            result = 0;
        } else {
            result = errno;
        }
    } else {
        result = 0;
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

void KqueueBackend::async_recvfrom(int fd, const iovec* bufs, int buf_count,
                                  ActorId actor, uint32_t op_type) {
    struct sockaddr_storage addr;
    socklen_t addrlen = sizeof(addr);

    ssize_t n = ::recvfrom(fd, nullptr, 0, MSG_PEEK, reinterpret_cast<struct sockaddr*>(&addr), &addrlen);
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

    // Actually read data into the provided buffers
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

void KqueueBackend::async_sendto(int fd, const iovec* bufs, int buf_count,
                                 const sockaddr* addr, socklen_t addrlen,
                                 ActorId actor, uint32_t op_type) {
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

    ssize_t n = ::sendto(fd, data.data(), data.size(), 0, addr, addrlen);
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

int KqueueBackend::wait(int timeout_ms) {
    struct timespec ts;
    ts.tv_sec = timeout_ms / 1000;
    ts.tv_nsec = (timeout_ms % 1000) * 1000000;

    struct kevent events[16];
    int num_events = kevent(kqueue_fd_, nullptr, 0, events, 16, &ts);

    if (num_events < 0) {
        if (errno == EINTR) {
            return 0;
        }
        return -1;
    }

    // Process timer expirations
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    int64_t now_ms = now.tv_sec * 1000 + now.tv_nsec / 1000000;

    for (int i = 0; i < num_events; ++i) {
        if (events[i].filter == EVFILT_TIMER) {
            uint64_t handle = static_cast<uint64_t>(events[i].ident);
            if (cancelled_timers_.count(handle) == 0) {
                // Find the timer
                for (auto& timer : timers_) {
                    if (timer.handle == handle) {
                        OpCompletion completion{
                            .actor = timer.actor,
                            .type = OpType::TimerFired,
                            .fd = -1,
                            .result = 0,
                            .user_data = handle,
                        };
                        deliver_completion(completion);

                        // Reschedule if repeating
                        if (timer.interval_ms > 0) {
                            timer.expires_at_ms = now_ms + timer.interval_ms;
                        }
                        break;
                    }
                }
            }
            cancelled_timers_.erase(handle);
        }
    }

    // Remove expired one-shot timers
    timers_.erase(
        std::remove_if(timers_.begin(), timers_.end(),
                       [now_ms](const TimerEntry& t) {
                           return t.expires_at_ms < now_ms && t.interval_ms == 0;
                       }),
        timers_.end()
    );

    return num_events;
}

void KqueueBackend::process_completions() {
    // Completions are delivered immediately in kqueue backend
}

void KqueueBackend::deliver_completion(OpCompletion completion) {
    if (loop_) {
        loop_->enqueue_completion(completion);
    }
}

#else // !defined(__APPLE__) && !defined(__FreeBSD__) ...

// Stub implementations for non-BSD

KqueueBackend::KqueueBackend() = default;
KqueueBackend::~KqueueBackend() = default;

bool KqueueBackend::start() { return false; }
void KqueueBackend::stop() {}

bool KqueueBackend::add_fd(int fd, IoEvent events) {
    (void)fd; (void)events;
    return false;
}
bool KqueueBackend::update_fd(int fd, IoEvent events) {
    (void)fd; (void)events;
    return false;
}
bool KqueueBackend::remove_fd(int fd) {
    (void)fd;
    return false;
}

int KqueueBackend::register_buffer(const void* addr, size_t len) {
    (void)addr; (void)len;
    return -1;
}
bool KqueueBackend::unregister_buffer(int buffer_id) {
    (void)buffer_id;
    return false;
}

void KqueueBackend::async_send(int fd, const iovec* bufs, int buf_count,
                               ActorId actor, uint32_t op_type) {
    (void)fd; (void)bufs; (void)buf_count; (void)actor; (void)op_type;
}
void KqueueBackend::async_recv(int fd, const iovec* bufs, int buf_count,
                               ActorId actor, uint32_t op_type) {
    (void)fd; (void)bufs; (void)buf_count; (void)actor; (void)op_type;
}

void KqueueBackend::async_send_fixed(int fd, int buffer_id, size_t offset, size_t len,
                                     ActorId actor, uint32_t op_type) {
    (void)fd; (void)buffer_id; (void)offset; (void)len; (void)actor; (void)op_type;
}
void KqueueBackend::async_recv_fixed(int fd, int buffer_id, size_t offset, size_t len,
                                     ActorId actor, uint32_t op_type) {
    (void)fd; (void)buffer_id; (void)offset; (void)len; (void)actor; (void)op_type;
}

void KqueueBackend::async_accept(int fd, ActorId actor) {
    (void)fd; (void)actor;
}
void KqueueBackend::async_connect(int fd, const sockaddr* addr, socklen_t addrlen,
                                  ActorId actor) {
    (void)fd; (void)addr; (void)addrlen; (void)actor;
}

void KqueueBackend::async_recvfrom(int fd, const iovec* bufs, int buf_count,
                                   ActorId actor, uint32_t op_type) {
    (void)fd; (void)bufs; (void)buf_count; (void)actor; (void)op_type;
}
void KqueueBackend::async_sendto(int fd, const iovec* bufs, int buf_count,
                                 const sockaddr* addr, socklen_t addrlen,
                                 ActorId actor, uint32_t op_type) {
    (void)fd; (void)bufs; (void)buf_count; (void)addr; (void)addrlen; (void)actor; (void)op_type;
}

uint64_t KqueueBackend::run_after(ActorId actor, int delay_ms) {
    (void)actor; (void)delay_ms;
    return 0;
}
uint64_t KqueueBackend::run_every(ActorId actor, int interval_ms) {
    (void)actor; (void)interval_ms;
    return 0;
}
void KqueueBackend::cancel_timer(uint64_t handle) {
    (void)handle;
}

int KqueueBackend::wait(int timeout_ms) {
    (void)timeout_ms;
    return -1;
}
void KqueueBackend::process_completions() {}

void KqueueBackend::deliver_completion(OpCompletion completion) {
    (void)completion;
}

#endif // defined(__APPLE__) || defined(__FreeBSD__) ...

} // namespace net
} // namespace hpactor
