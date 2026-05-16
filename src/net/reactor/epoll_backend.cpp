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
#include <hpactor/net/reactor/epoll_backend.hpp>

#if defined(__linux__)
#    include <cerrno>
#    include <cstdlib>
#    include <cstring>
#    include <ctime>
#    include <fcntl.h>
#    include <sys/epoll.h>
#    include <sys/socket.h>
#    include <sys/timerfd.h>
#    include <sys/uio.h>
#    include <unistd.h>
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
    ev.data.fd = timerfd_;
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
    pending_ops_.clear();
}

bool EpollBackend::add_fd(int fd, IoEvent events) {
    uint32_t epoll_events = 0;
    using T = std::underlying_type_t<IoEvent>;
    if (static_cast<T>(events) & static_cast<T>(IoEvent::Read)) {
        epoll_events |= EPOLLIN;
    }
    if (static_cast<T>(events) & static_cast<T>(IoEvent::Write)) {
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
    return add_fd(fd, events); // Same operation
}

bool EpollBackend::remove_fd(int fd) {
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr) < 0) {
        return false;
    }
    fd_events_.erase(fd);
    pending_ops_.erase(fd);
    read_handlers_.erase(fd);
    write_handlers_.erase(fd);
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

void EpollBackend::decode_user_data(uint64_t ud, int& fd, ActorId& actor,
                                    uint32_t& op_type) {
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
                    timer.expires_at_ms = -1; // Mark for removal
                }
            }
        }
    }

    // Remove expired one-shot timers
    timers_.erase(
        std::remove_if(timers_.begin(), timers_.end(),
                       [](const TimerEntry& t) { return t.expires_at_ms < 0; }),
        timers_.end());

    // Sort remaining timers by expiry
    std::sort(timers_.begin(), timers_.end(),
              [](const TimerEntry& a, const TimerEntry& b) {
                  return a.expires_at_ms < b.expires_at_ms;
              });

    // Update timerfd to next expiry if we have timers
    if (!timers_.empty()) {
        int64_t nextExpiry = timers_.front().expires_at_ms;
        struct timespec now_ts;
        clock_gettime(CLOCK_MONOTONIC, &now_ts);
        int64_t now_abs_ms = now_ts.tv_sec * 1000 + now_ts.tv_nsec / 1000000;
        int64_t delay_ms = nextExpiry - now_abs_ms;
        if (delay_ms < 1)
            delay_ms = 1;
        struct itimerspec new_val{};
        new_val.it_value.tv_sec = delay_ms / 1000;
        new_val.it_value.tv_nsec = (delay_ms % 1000) * 1000000;
        timerfd_settime(timerfd_, 0, &new_val, nullptr);
    } else {
        // No timers, disarm
        struct itimerspec new_val{};
        timerfd_settime(timerfd_, 0, &new_val, nullptr);
    }

    return triggered;
}

// --- Sub-methods extracted from process_pending_op ---

void EpollBackend::try_pending_accept(int fd, PendingOp& op) {
    while (true) {
        int client_fd = ::accept(fd, nullptr, nullptr);
        if (client_fd < 0) {
            if (errno == EAGAIN)
                break;
            break;
        }
        add_fd(client_fd, IoEvent::Read);

        OpCompletion completion{
            .actor = op.actor,
            .type = OpType::Accept,
            .fd = client_fd,
            .result = client_fd,
            .user_data = 0,
        };
        {
            std::lock_guard<std::mutex> lock(completions_mutex_);
            pending_completions_.push_back(completion);
        }
    }
}

bool EpollBackend::try_pending_connect(int fd, PendingOp& op, uint32_t events,
                                       int& error) {
    (void)fd;
    (void)op;
    if (!(events & EPOLLOUT))
        return false;
    int err = 0;
    socklen_t errlen = sizeof(err);
    getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &errlen);
    error = err;
    return true;
}

bool EpollBackend::try_pending_send(int fd, PendingOp& op, uint32_t events,
                                    ssize_t& total, int& error) {
    if (!(events & EPOLLOUT))
        return false;

    OpType optype = static_cast<OpType>(op.op_type);
    while (true) {
        ssize_t n;
        if (optype == OpType::SendTo) {
            n = ::sendto(fd, op.data.data(), op.data.size(), 0,
                         reinterpret_cast<const sockaddr*>(&op.addr), op.addrlen);
        } else {
            n = ::send(fd, op.data.data(), op.data.size(), 0);
        }
        if (n < 0) {
            if (errno == EAGAIN)
                return false; // Keep pending op
            error = errno;
            return true;
        }
        total += n;
        if (n < static_cast<ssize_t>(op.data.size())) {
            op.data.erase(op.data.begin(), op.data.begin() + n);
        } else {
            return true; // All data sent
        }
    }
}

bool EpollBackend::try_pending_recv(int fd, PendingOp& op, uint32_t events,
                                    ssize_t& total, int& error) {
    if (!(events & EPOLLIN))
        return false;

    OpType optype = static_cast<OpType>(op.op_type);
    while (true) {
        ssize_t n;
        if (optype == OpType::RecvFrom) {
            struct msghdr hdr = {};
            hdr.msg_iov = op.saved_bufs;
            hdr.msg_iovlen = static_cast<decltype(hdr.msg_iovlen)>(op.buf_count);
            n = ::recvmsg(fd, &hdr, 0);
        } else {
            n = ::readv(fd, op.saved_bufs, op.buf_count);
        }
        if (n < 0) {
            if (errno == EAGAIN)
                return false; // Keep pending op
            error = errno;
            return true;
        }
        total += n;
        if (n == 0)
            return true; // EOF
    }
}

void EpollBackend::deliver_op_completion(const PendingOp& op, OpType optype,
                                         int fd, ssize_t total, int error) {
    OpCompletion completion{
        .actor = op.actor,
        .type = optype,
        .fd = fd,
        .result = (error != 0) ? -error : static_cast<int>(total),
        .user_data = 0,
    };
    {
        std::lock_guard<std::mutex> lock(completions_mutex_);
        pending_completions_.push_back(completion);
    }
}

// --- Dispatcher ---

void EpollBackend::process_pending_op(int fd, uint32_t events) {
    auto it = pending_ops_.find(fd);
    if (it == pending_ops_.end())
        return;

    PendingOp& op = it->second;
    OpType optype = static_cast<OpType>(op.op_type);

    switch (optype) {
        case OpType::Accept:
            try_pending_accept(fd, op);
            return; // Keep pending op for re-trigger

        case OpType::Connect: {
            int error = 0;
            if (try_pending_connect(fd, op, events, error)) {
                deliver_op_completion(op, optype, fd, 0, error);
                pending_ops_.erase(it);
            }
            return;
        }
        case OpType::Send:
        case OpType::SendTo: {
            ssize_t total = 0;
            int error = 0;
            if (try_pending_send(fd, op, events, total, error)) {
                deliver_op_completion(op, optype, fd, total, error);
                pending_ops_.erase(it);
            }
            return;
        }
        case OpType::Recv:
        case OpType::RecvFrom: {
            ssize_t total = 0;
            int error = 0;
            if (try_pending_recv(fd, op, events, total, error)) {
                deliver_op_completion(op, optype, fd, total, error);
                pending_ops_.erase(it);
            }
            return;
        }
        default:
            deliver_op_completion(op, optype, fd, 0, EINVAL);
            pending_ops_.erase(it);
            return;
    }
}

// --- Event dispatch ---

void EpollBackend::dispatch_event(int fd, uint32_t events) {
    if (fd == timerfd_) {
        process_timers();
    } else if (fd >= 0) {
        if (pending_ops_.find(fd) != pending_ops_.end()) {
            process_pending_op(fd, events);
        } else {
            if (events & EPOLLIN) {
                service_read_handler(fd);
            }
            if (events & EPOLLOUT) {
                service_write_handler(fd);
            }
        }
    }
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

    // Update timerfd to fire at earliest timer (relative delay from now)
    int64_t front_delay = timers_.front().expires_at_ms -
                          (ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
    if (front_delay < 1)
        front_delay = 1;
    struct itimerspec new_val{};
    new_val.it_value.tv_sec = front_delay / 1000;
    new_val.it_value.tv_nsec = (front_delay % 1000) * 1000000;
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

    // Update timerfd to fire at earliest timer (relative delay from now)
    int64_t front_delay = timers_.front().expires_at_ms -
                          (ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
    if (front_delay < 1)
        front_delay = 1;
    struct itimerspec new_val{};
    new_val.it_value.tv_sec = front_delay / 1000;
    new_val.it_value.tv_nsec = (front_delay % 1000) * 1000000;
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
    // Concatenate scatter-gather buffers
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

    size_t total_n = 0;
    int last_err = 0;

    // Loop until EAGAIN to drain send buffer completely (edge-triggered
    // requirement)
    while (true) {
        ssize_t n = ::send(fd, data.data() + total_n, data.size() - total_n, 0);
        if (n < 0) {
            if (errno == EAGAIN) {
                break; // Would block
            }
            last_err = errno;
            break;
        }
        total_n += static_cast<size_t>(n);
        if (total_n >= data.size()) {
            break; // All data sent
        }
        // Continue looping to send more
    }

    if (total_n == 0 && last_err == 0) {
        // Would block - store pending operation for wait() to process
        PendingOp op;
        op.actor = actor;
        op.op_type = op_type;
        op.data = std::move(data);
        op.buf_count = buf_count;
        pending_ops_[fd] = std::move(op);
        return;
    }

    int result = (last_err != 0) ? last_err : static_cast<int>(total_n);

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

void EpollBackend::async_recv(int fd, const iovec* bufs, int buf_count,
                              ActorId actor, uint32_t op_type) {
    ssize_t total_n = 0;
    int last_err = 0;

    // Loop until EAGAIN to drain all available data (edge-triggered
    // requirement)
    while (true) {
        ssize_t n = ::readv(fd, bufs, buf_count);
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

    if (total_n == 0 && last_err == 0) {
        // Socket not ready - store pending operation for wait() to process
        PendingOp op;
        op.actor = actor;
        op.op_type = op_type;
        op.buf_count = std::min(buf_count, 16);
        for (int i = 0; i < op.buf_count; ++i) {
            op.saved_bufs[i] = bufs[i];
        }
        pending_ops_[fd] = std::move(op);
        return;
    }

    int result = (last_err != 0) ? last_err : static_cast<int>(total_n);

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

void EpollBackend::async_send_fixed(int fd, int buffer_id, size_t offset,
                                    size_t len, ActorId actor, uint32_t op_type) {
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
    {
        std::lock_guard<std::mutex> lock(completions_mutex_);
        pending_completions_.push_back(completion);
    }
}

void EpollBackend::async_recv_fixed(int fd, int buffer_id, size_t offset,
                                    size_t len, ActorId actor, uint32_t op_type) {
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
    {
        std::lock_guard<std::mutex> lock(completions_mutex_);
        pending_completions_.push_back(completion);
    }
}

void EpollBackend::async_accept(int fd, ActorId actor) {
    int client_fd = ::accept(fd, nullptr, nullptr);
    if (client_fd < 0 && errno == EAGAIN) {
        // Would block - register fd with epoll for edge-triggered accept
        // then store pending accept for wait() to process
        struct epoll_event ev;
        ev.events = EPOLLIN | EPOLLET;
        ev.data.fd = fd;
        if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) < 0) {
            // Failed to register - deliver error completion
            OpCompletion completion{
                .actor = actor,
                .type = OpType::Accept,
                .fd = -1,
                .result = -errno,
                .user_data = 0,
            };
            {
                std::lock_guard<std::mutex> lock(completions_mutex_);
                pending_completions_.push_back(completion);
            }
            return;
        }
        PendingOp op;
        op.actor = actor;
        op.op_type = static_cast<uint32_t>(OpType::Accept);
        pending_ops_[fd] = std::move(op);
        return;
    }

    if (client_fd >= 0) {
        // Register client_fd with event loop before delivering completion
        // so caller can immediately use it with async_recv
        add_fd(client_fd, IoEvent::Read);
    }

    OpCompletion completion{
        .actor = actor,
        .type = OpType::Accept,
        .fd = (client_fd >= 0) ? client_fd : -1,
        .result = (client_fd >= 0) ? client_fd : errno,
        .user_data = 0,
    };
    {
        std::lock_guard<std::mutex> lock(completions_mutex_);
        pending_completions_.push_back(completion);
    }
}

void EpollBackend::async_connect(int fd, const sockaddr* addr,
                                 socklen_t addrlen, ActorId actor) {
    int ret = ::connect(fd, addr, addrlen);
    if (ret < 0 && errno == EINPROGRESS) {
        // Connection in progress - store for wait() to complete
        PendingOp op;
        op.actor = actor;
        op.op_type = static_cast<uint32_t>(OpType::Connect);
        if (addrlen <= sizeof(op.addr)) {
            std::memcpy(&op.addr, addr, addrlen);
            op.addrlen = addrlen;
        }
        pending_ops_[fd] = std::move(op);
        return;
    }

    int result = (ret < 0) ? errno : 0;

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

void EpollBackend::async_recvfrom(int fd, const iovec* bufs, int buf_count,
                                  ActorId actor, uint32_t op_type) {
    struct sockaddr_storage addr;
    socklen_t addrlen = sizeof(addr);
    ssize_t total_n = 0;
    int last_err = 0;

    struct msghdr msg = {};
    msg.msg_name = &addr;
    msg.msg_namelen = addrlen;
    msg.msg_iov = const_cast<iovec*>(bufs);
    msg.msg_iovlen = static_cast<decltype(msg.msg_iovlen)>(buf_count);

    // Loop until EAGAIN to drain all available data (edge-triggered
    // requirement)
    while (true) {
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

    if (total_n == 0 && last_err == 0) {
        // Socket not ready - store pending for wait() to process
        PendingOp op;
        op.actor = actor;
        op.op_type = op_type;
        op.buf_count = std::min(buf_count, 16);
        for (int i = 0; i < op.buf_count; ++i) {
            op.saved_bufs[i] = bufs[i];
        }
        pending_ops_[fd] = std::move(op);
        return;
    }

    int result = (last_err != 0) ? last_err : static_cast<int>(total_n);

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

void EpollBackend::async_sendto(int fd, const iovec* bufs, int buf_count,
                                const sockaddr* addr, socklen_t addrlen,
                                ActorId actor, uint32_t op_type) {
    // Concatenate scatter-gather buffers
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

    size_t total_n = 0;
    int last_err = 0;

    // Loop until EAGAIN to drain send buffer completely (edge-triggered
    // requirement)
    while (true) {
        ssize_t n = ::sendto(fd, data.data() + total_n, data.size() - total_n,
                             0, addr, addrlen);
        if (n < 0) {
            if (errno == EAGAIN) {
                break; // Would block
            }
            last_err = errno;
            break;
        }
        total_n += static_cast<size_t>(n);
        if (total_n >= data.size()) {
            break; // All data sent
        }
        // Continue looping to send more
    }

    if (total_n == 0 && last_err == 0) {
        // Would block - store pending for wait() to process
        PendingOp op;
        op.actor = actor;
        op.op_type = op_type;
        op.data = std::move(data);
        if (addrlen <= sizeof(op.addr)) {
            std::memcpy(&op.addr, addr, addrlen);
            op.addrlen = addrlen;
        }
        pending_ops_[fd] = std::move(op);
        return;
    }

    int result = (last_err != 0) ? last_err : static_cast<int>(total_n);

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

int EpollBackend::wait(int timeout_ms) {
    struct epoll_event events[16];
    int num_events = epoll_wait(epoll_fd_, events, 16, timeout_ms);

    if (num_events < 0) {
        if (errno == EINTR)
            return 0;
        return -1;
    }

    for (int i = 0; i < num_events; ++i) {
        dispatch_event(events[i].data.fd, events[i].events);
    }

    return num_events;
}

void EpollBackend::set_read_handler(int fd, read_callback handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (handler) {
        read_handlers_[fd] = std::move(handler);
    } else {
        read_handlers_.erase(fd);
    }
}

void EpollBackend::clear_read_handler(int fd) {
    std::lock_guard<std::mutex> lock(mutex_);
    read_handlers_.erase(fd);
}

void EpollBackend::service_read_handler(int fd) {
    read_callback cb;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = read_handlers_.find(fd);
        if (it == read_handlers_.end())
            return;
        cb = it->second;
    }
    cb(fd);
}

void EpollBackend::set_write_handler(int fd, write_callback handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (handler) {
        write_handlers_[fd] = std::move(handler);
    } else {
        write_handlers_.erase(fd);
    }
}

void EpollBackend::clear_write_handler(int fd) {
    std::lock_guard<std::mutex> lock(mutex_);
    write_handlers_.erase(fd);
}

void EpollBackend::service_write_handler(int fd) {
    write_callback cb;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = write_handlers_.find(fd);
        if (it == write_handlers_.end())
            return;
        cb = it->second;
    }
    cb(fd);
}

void EpollBackend::process_events() {
    std::vector<OpCompletion> completions;
    {
        std::lock_guard<std::mutex> lock(completions_mutex_);
        completions = std::move(pending_completions_);
        pending_completions_.clear();
    }
    for (auto& completion : completions) {
        deliver_completion(completion);
    }
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

bool EpollBackend::start() {
    return false;
}
void EpollBackend::stop() {}

bool EpollBackend::add_fd(int fd, IoEvent events) {
    (void)fd;
    (void)events;
    return false;
}
bool EpollBackend::update_fd(int fd, IoEvent events) {
    (void)fd;
    (void)events;
    return false;
}
bool EpollBackend::remove_fd(int fd) {
    (void)fd;
    return false;
}

int EpollBackend::register_buffer(const void* addr, size_t len) {
    (void)addr;
    (void)len;
    return -1;
}
bool EpollBackend::unregister_buffer(int buffer_id) {
    (void)buffer_id;
    return false;
}

void EpollBackend::async_send(int fd, const iovec* bufs, int buf_count,
                              ActorId actor, uint32_t op_type) {
    (void)fd;
    (void)bufs;
    (void)buf_count;
    (void)actor;
    (void)op_type;
}
void EpollBackend::async_recv(int fd, const iovec* bufs, int buf_count,
                              ActorId actor, uint32_t op_type) {
    (void)fd;
    (void)bufs;
    (void)buf_count;
    (void)actor;
    (void)op_type;
}

void EpollBackend::async_send_fixed(int fd, int buffer_id, size_t offset,
                                    size_t len, ActorId actor, uint32_t op_type) {
    (void)fd;
    (void)buffer_id;
    (void)offset;
    (void)len;
    (void)actor;
    (void)op_type;
}
void EpollBackend::async_recv_fixed(int fd, int buffer_id, size_t offset,
                                    size_t len, ActorId actor, uint32_t op_type) {
    (void)fd;
    (void)buffer_id;
    (void)offset;
    (void)len;
    (void)actor;
    (void)op_type;
}

void EpollBackend::async_accept(int fd, ActorId actor) {
    (void)fd;
    (void)actor;
}
void EpollBackend::async_connect(int fd, const sockaddr* addr,
                                 socklen_t addrlen, ActorId actor) {
    (void)fd;
    (void)addr;
    (void)addrlen;
    (void)actor;
}

void EpollBackend::async_recvfrom(int fd, const iovec* bufs, int buf_count,
                                  ActorId actor, uint32_t op_type) {
    (void)fd;
    (void)bufs;
    (void)buf_count;
    (void)actor;
    (void)op_type;
}
void EpollBackend::async_sendto(int fd, const iovec* bufs, int buf_count,
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

uint64_t EpollBackend::run_after(ActorId actor, int delay_ms) {
    (void)actor;
    (void)delay_ms;
    return 0;
}
uint64_t EpollBackend::run_every(ActorId actor, int interval_ms) {
    (void)actor;
    (void)interval_ms;
    return 0;
}
void EpollBackend::cancel_timer(uint64_t handle) {
    (void)handle;
}

int EpollBackend::wait(int timeout_ms) {
    (void)timeout_ms;
    return -1;
}
void EpollBackend::process_events() {}

void EpollBackend::deliver_completion(OpCompletion completion) {
    (void)completion;
}

void EpollBackend::set_write_handler(int fd, write_callback handler) {
    (void)fd;
    (void)handler;
}
void EpollBackend::clear_write_handler(int fd) {
    (void)fd;
}
void EpollBackend::service_write_handler(int fd) {
    (void)fd;
}

#endif // defined(__linux__)

} // namespace net
} // namespace hpactor
