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

#include <hpactor/net/event_loop.hpp>
#include <hpactor/net/kqueue_backend.hpp>

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) ||      \
    defined(__NetBSD__)
#    include <cstdlib>
#    include <cstring>
#    include <errno.h>
#    include <fcntl.h>
#    include <sys/event.h>
#    include <sys/socket.h>
#    include <sys/time.h>
#    include <sys/uio.h>
#    include <unistd.h>
#endif

namespace hpactor {
namespace net {

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) ||      \
    defined(__NetBSD__)

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

void KqueueBackend::decode_user_data(uint64_t ud, int& fd, ActorId& actor,
                                     uint32_t& op_type) {
    fd = static_cast<int>(ud & 0xFFFFFFFFULL);
    actor = ActorId(static_cast<uint32_t>((ud >> 32) & 0xFFFFULL));
    op_type = static_cast<uint32_t>((ud >> 56) & 0xFFULL);
}

uint64_t KqueueBackend::run_after(ActorId actor, int delay_ms) {
    std::lock_guard<std::mutex> lock(mutex_);

    uint64_t handle = next_timer_handle_++;

    // Register timer with kqueue using EVFILT_TIMER
    // On macOS: fflags = NOTE_USECONDS means data is in microseconds
    struct kevent ev;
    EV_SET(&ev, handle, EVFILT_TIMER, EV_ADD | EV_ONESHOT, NOTE_USECONDS,
           delay_ms * 1000, 0);
    if (kevent(kqueue_fd_, &ev, 1, nullptr, 0, nullptr) < 0) {
        return 0; // Failed to register
    }

    // Store timer info for lookup when it fires
    TimerEntry entry;
    entry.expires_at_ms = 0; // Not used when using EVFILT_TIMER
    entry.actor = actor;
    entry.interval_ms = 0;
    entry.handle = handle;

    timers_.push_back(entry);

    return handle;
}

uint64_t KqueueBackend::run_every(ActorId actor, int interval_ms) {
    std::lock_guard<std::mutex> lock(mutex_);

    uint64_t handle = next_timer_handle_++;

    // For repeating timers, we need to re-register after each fire
    // Store interval for use when rescheduling
    TimerEntry entry;
    entry.expires_at_ms = 0;
    entry.actor = actor;
    entry.interval_ms = interval_ms;
    entry.handle = handle;

    // Register first occurrence with kqueue
    struct kevent ev;
    EV_SET(&ev, handle, EVFILT_TIMER, EV_ADD | EV_ONESHOT, NOTE_USECONDS,
           interval_ms * 1000, 0);
    if (kevent(kqueue_fd_, &ev, 1, nullptr, 0, nullptr) < 0) {
        return 0; // Failed to register
    }

    timers_.push_back(entry);

    return handle;
}

void KqueueBackend::cancel_timer(uint64_t handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    cancelled_timers_.insert(handle);

    // Remove timer from kqueue if it's still registered
    struct kevent ev;
    EV_SET(&ev, handle, EVFILT_TIMER, EV_DELETE, 0, 0, 0);
    kevent(kqueue_fd_, &ev, 1, nullptr, 0, nullptr);
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
    int result = (n < 0) ? -errno : static_cast<int>(n);

    OpCompletion completion{
        .actor = actor,
        .type = static_cast<OpType>(op_type),
        .fd = fd,
        .result = result,
        .user_data = 0,
    };
    {
        std::lock_guard<std::mutex> lock(completions_mutex_);
        pending_completions_.push_back(completion);
    }
}

void KqueueBackend::async_recv(int fd, const iovec* bufs, int buf_count,
                               ActorId actor, uint32_t op_type) {
    ssize_t n = ::readv(fd, bufs, buf_count);
    int result = (n < 0) ? -errno : static_cast<int>(n);

    OpCompletion completion{
        .actor = actor,
        .type = static_cast<OpType>(op_type),
        .fd = fd,
        .result = result,
        .user_data = 0,
    };
    {
        std::lock_guard<std::mutex> lock(completions_mutex_);
        pending_completions_.push_back(completion);
    }
}

void KqueueBackend::async_send_fixed(int fd, int buffer_id, size_t offset,
                                     size_t len, ActorId actor, uint32_t op_type) {
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
    {
        std::lock_guard<std::mutex> lock(completions_mutex_);
        pending_completions_.push_back(completion);
    }
}

void KqueueBackend::async_recv_fixed(int fd, int buffer_id, size_t offset,
                                     size_t len, ActorId actor, uint32_t op_type) {
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
    {
        std::lock_guard<std::mutex> lock(completions_mutex_);
        pending_completions_.push_back(completion);
    }
}

void KqueueBackend::async_accept(int fd, ActorId actor) {
    int client_fd = ::accept(fd, nullptr, nullptr);
    OpCompletion completion{
        .actor = actor,
        .type = OpType::Accept,
        .fd = (client_fd >= 0) ? client_fd : -1,
        .result = (client_fd >= 0) ? client_fd : -errno,
        .user_data = 0,
    };
    {
        std::lock_guard<std::mutex> lock(completions_mutex_);
        pending_completions_.push_back(completion);
    }
}

void KqueueBackend::async_connect(int fd, const sockaddr* addr,
                                  socklen_t addrlen, ActorId actor) {
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
    {
        std::lock_guard<std::mutex> lock(completions_mutex_);
        pending_completions_.push_back(completion);
    }
}

void KqueueBackend::async_recvfrom(int fd, const iovec* bufs, int buf_count,
                                   ActorId actor, uint32_t op_type) {
    struct sockaddr_storage addr;
    socklen_t addrlen = sizeof(addr);

    // Ensure fd is non-blocking for event-driven I/O
    int flags = fcntl(fd, F_GETFL, 0);
    bool was_blocking = !(flags & O_NONBLOCK);
    if (was_blocking) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }

    // First check if data is available with non-blocking peek
    ssize_t peek_n =
        ::recvfrom(fd, nullptr, 0, MSG_PEEK,
                   reinterpret_cast<struct sockaddr*>(&addr), &addrlen);
    if (peek_n < 0 && errno == EAGAIN) {
        // No data available right now - restore blocking and deliver EAGAIN
        if (was_blocking) {
            fcntl(fd, F_SETFL, flags);
        }
        OpCompletion completion{
            .actor = actor,
            .type = static_cast<OpType>(op_type),
            .fd = fd,
            .result = -EAGAIN,
            .user_data = 0,
        };
        {
            std::lock_guard<std::mutex> lock(completions_mutex_);
            pending_completions_.push_back(completion);
        }
        return;
    }

    // Data is available or EOF. Read it.
    ssize_t total_n = 0;
    int last_err = 0;

    // Loop until EAGAIN to drain all available data (edge-triggered
    // requirement)
    while (true) {
        struct msghdr msg;
        msg.msg_name = &addr;
        msg.msg_namelen = addrlen;
        msg.msg_iov = const_cast<iovec*>(bufs);
        msg.msg_iovlen = buf_count;
        msg.msg_control = nullptr;
        msg.msg_controllen = 0;
        msg.msg_flags = 0;

        ssize_t n = ::recvmsg(fd, &msg, 0);
        if (n < 0) {
            if (errno == EAGAIN) {
                break; // No more data available right now
            }
            last_err = errno;
            break;
        }
        total_n += n;
        if (n == 0) {
            break; // EOF
        }
        // Continue looping to check for more data
    }

    // Restore blocking mode if we changed it
    if (was_blocking) {
        fcntl(fd, F_SETFL, flags);
    }

    int result = (last_err != 0) ? -last_err : static_cast<int>(total_n);

    OpCompletion completion{
        .actor = actor,
        .type = static_cast<OpType>(op_type),
        .fd = fd,
        .result = result,
        .user_data = 0,
    };
    {
        std::lock_guard<std::mutex> lock(completions_mutex_);
        pending_completions_.push_back(completion);
    }
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
    int result = (n < 0) ? -errno : static_cast<int>(n);

    OpCompletion completion{
        .actor = actor,
        .type = static_cast<OpType>(op_type),
        .fd = fd,
        .result = result,
        .user_data = 0,
    };
    {
        std::lock_guard<std::mutex> lock(completions_mutex_);
        pending_completions_.push_back(completion);
    }
}

int KqueueBackend::wait(int timeout_ms) {
    struct timespec ts;
    ts.tv_sec = timeout_ms / 1000;
    ts.tv_nsec = (timeout_ms % 1000) * 1000000;

    struct kevent events[16];
    int num_events = kevent(kqueue_fd_, nullptr, 0, events, 16, &ts);

    // Store events for process_completions to handle
    if (num_events > 0 && num_events <= 16) {
        for (int i = 0; i < num_events; ++i) {
            events_[i] = events[i];
        }
        last_num_events_ = num_events;
    } else {
        last_num_events_ = 0;
    }

    if (num_events < 0) {
        if (errno == EINTR) {
            return 0;
        }
        return -1;
    }

    // Process timer expirations
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

                        // Reschedule repeating timers by re-registering with
                        // kqueue
                        if (timer.interval_ms > 0) {
                            struct kevent ev;
                            EV_SET(&ev, handle, EVFILT_TIMER, EV_ADD | EV_ONESHOT,
                                   NOTE_USECONDS, timer.interval_ms * 1000, 0);
                            kevent(kqueue_fd_, &ev, 1, nullptr, 0, nullptr);
                        }
                        break;
                    }
                }
            }
            cancelled_timers_.erase(handle);
        }
    }

    // Clean up cancelled one-shot timers
    timers_.erase(std::remove_if(timers_.begin(), timers_.end(),
                                 [this](const TimerEntry& t) {
                                     return cancelled_timers_.count(t.handle) > 0 &&
                                            t.interval_ms == 0;
                                 }),
                  timers_.end());

    // Process pending socket I/O operations
    for (int i = 0; i < num_events; ++i) {
        int fd = static_cast<int>(events[i].ident);

        // Check if we have a pending operation for this fd
        auto it = pending_ops_.find(fd);
        if (it != pending_ops_.end()) {
            PendingOp& op = it->second;
            OpType optype = static_cast<OpType>(op.op_type);

            if (events[i].filter == EVFILT_READ &&
                (optype == OpType::Recv || optype == OpType::RecvFrom)) {
                // Process pending recv
                ssize_t total_n = 0;
                int last_err = 0;

                while (true) {
                    struct msghdr msg;
                    msg.msg_name = nullptr;
                    msg.msg_namelen = 0;
                    msg.msg_iov = op.saved_bufs;
                    msg.msg_iovlen = op.buf_count;
                    msg.msg_control = nullptr;
                    msg.msg_controllen = 0;
                    msg.msg_flags = 0;

                    ssize_t n = ::recvmsg(fd, &msg, MSG_DONTWAIT);
                    if (n < 0) {
                        if (errno == EAGAIN) {
                            // No more data, keep pending op for next trigger
                            goto next_event;
                        }
                        last_err = errno;
                        break;
                    }
                    total_n += n;
                    if (n == 0) {
                        break; // EOF
                    }
                }

                if (total_n > 0 || last_err != 0) {
                    int result =
                        (last_err != 0) ? -last_err : static_cast<int>(total_n);
                    OpCompletion completion{
                        .actor = op.actor,
                        .type = optype,
                        .fd = fd,
                        .result = result,
                        .user_data = 0,
                    };
                    {
                        std::lock_guard<std::mutex> lock(completions_mutex_);
                        pending_completions_.push_back(completion);
                    }
                    pending_ops_.erase(it);
                }
            } else if (events[i].filter == EVFILT_WRITE &&
                       (optype == OpType::Send || optype == OpType::SendTo)) {
                // Process pending send
                ssize_t total_n = 0;
                int last_err = 0;

                while (true) {
                    ssize_t n;
                    if (optype == OpType::SendTo) {
                        n = ::sendto(fd, op.data.data() + total_n,
                                     op.data.size() - static_cast<size_t>(total_n),
                                     MSG_DONTWAIT,
                                     reinterpret_cast<const sockaddr*>(&op.addr),
                                     op.addrlen);
                    } else {
                        n = ::send(fd, op.data.data() + total_n,
                                   op.data.size() - static_cast<size_t>(total_n),
                                   MSG_DONTWAIT);
                    }
                    if (n < 0) {
                        if (errno == EAGAIN) {
                            // Would block, keep pending op
                            goto next_event;
                        }
                        last_err = errno;
                        break;
                    }
                    total_n += n;
                    if (static_cast<size_t>(total_n) >= op.data.size()) {
                        break; // All sent
                    }
                }

                if (total_n > 0 || last_err != 0) {
                    int result =
                        (last_err != 0) ? -last_err : static_cast<int>(total_n);
                    OpCompletion completion{
                        .actor = op.actor,
                        .type = optype,
                        .fd = fd,
                        .result = result,
                        .user_data = 0,
                    };
                    {
                        std::lock_guard<std::mutex> lock(completions_mutex_);
                        pending_completions_.push_back(completion);
                    }
                    pending_ops_.erase(it);
                }
            }
        }
    next_event:
        continue;
    }

    return num_events;
}

void KqueueBackend::process_completions() {
    // Process pending completions from async_* calls
    std::vector<OpCompletion> completions;
    {
        std::lock_guard<std::mutex> lock(completions_mutex_);
        completions = std::move(pending_completions_);
        pending_completions_.clear();
    }
    for (auto& completion : completions) {
        deliver_completion(completion);
    }

    last_num_events_ = 0;
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

bool KqueueBackend::start() {
    return false;
}
void KqueueBackend::stop() {}

bool KqueueBackend::add_fd(int fd, IoEvent events) {
    (void)fd;
    (void)events;
    return false;
}
bool KqueueBackend::update_fd(int fd, IoEvent events) {
    (void)fd;
    (void)events;
    return false;
}
bool KqueueBackend::remove_fd(int fd) {
    (void)fd;
    return false;
}

int KqueueBackend::register_buffer(const void* addr, size_t len) {
    (void)addr;
    (void)len;
    return -1;
}
bool KqueueBackend::unregister_buffer(int buffer_id) {
    (void)buffer_id;
    return false;
}

void KqueueBackend::async_send(int fd, const iovec* bufs, int buf_count,
                               ActorId actor, uint32_t op_type) {
    (void)fd;
    (void)bufs;
    (void)buf_count;
    (void)actor;
    (void)op_type;
}
void KqueueBackend::async_recv(int fd, const iovec* bufs, int buf_count,
                               ActorId actor, uint32_t op_type) {
    (void)fd;
    (void)bufs;
    (void)buf_count;
    (void)actor;
    (void)op_type;
}

void KqueueBackend::async_send_fixed(int fd, int buffer_id, size_t offset,
                                     size_t len, ActorId actor, uint32_t op_type) {
    (void)fd;
    (void)buffer_id;
    (void)offset;
    (void)len;
    (void)actor;
    (void)op_type;
}
void KqueueBackend::async_recv_fixed(int fd, int buffer_id, size_t offset,
                                     size_t len, ActorId actor, uint32_t op_type) {
    (void)fd;
    (void)buffer_id;
    (void)offset;
    (void)len;
    (void)actor;
    (void)op_type;
}

void KqueueBackend::async_accept(int fd, ActorId actor) {
    (void)fd;
    (void)actor;
}
void KqueueBackend::async_connect(int fd, const sockaddr* addr,
                                  socklen_t addrlen, ActorId actor) {
    (void)fd;
    (void)addr;
    (void)addrlen;
    (void)actor;
}

void KqueueBackend::async_recvfrom(int fd, const iovec* bufs, int buf_count,
                                   ActorId actor, uint32_t op_type) {
    (void)fd;
    (void)bufs;
    (void)buf_count;
    (void)actor;
    (void)op_type;
}
void KqueueBackend::async_sendto(int fd, const iovec* bufs, int buf_count,
                                 const sockaddr* addr, socklen_t addrlen,
                                 ActorId actor, uint32_t op_type) {
    (void)fd;
    (void)bufs;
    (void)buf_count;
    (void)addr;
    (void)addrlen;
    (void)actor;
    (void)op_type;
}

uint64_t KqueueBackend::run_after(ActorId actor, int delay_ms) {
    (void)actor;
    (void)delay_ms;
    return 0;
}
uint64_t KqueueBackend::run_every(ActorId actor, int interval_ms) {
    (void)actor;
    (void)interval_ms;
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
