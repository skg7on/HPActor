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

#include <hpactor/net/iouring_backend.hpp>

#if defined(__linux__)
#include <liburing.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>
#endif

namespace hpactor {
namespace net {

#if defined(__linux__)

IoUringBackend::IoUringBackend() : ring_{}, running_(false) {}

IoUringBackend::~IoUringBackend() {
    stop();
}

bool IoUringBackend::start() {
    struct io_uring_params params = {};
    params.flags |= IORING_SETUP_SQPOLL;
    int ret = io_uring_queue_init_params(256, &ring_, &params);
    if (ret < 0) {
        return false;
    }
    running_ = true;
    return true;
}

void IoUringBackend::stop() {
    if (running_) {
        io_uring_queue_exit(&ring_);
        running_ = false;
    }
}

bool IoUringBackend::add_fd(int fd, IoEvent events) {
    // Register fd with io_uring if supported
    unsigned registered_index = static_cast<unsigned>(registered_fds_.size());
    int ret = io_uring_register_files(&ring_, &fd, 1);
    if (ret < 0) {
        // Fall back to unregistered mode - store fd for tracking only
        registered_fds_[fd] = registered_index;
        return true;
    }
    registered_fds_[fd] = registered_index;
    return true;
}

bool IoUringBackend::update_fd(int fd, IoEvent events) {
    // io_uring auto-updates socket interest; nothing to do
    (void)fd;
    (void)events;
    return true;
}

bool IoUringBackend::remove_fd(int fd) {
    auto it = registered_fds_.find(fd);
    if (it != registered_fds_.end()) {
        registered_fds_.erase(it);
        return true;
    }
    return false;
}

int IoUringBackend::register_buffer(const void* addr, size_t len) {
    struct iovec iov{const_cast<void*>(addr), len};
    int buffer_id = static_cast<int>(registered_buffers_.size());
    int ret = io_uring_register_buffers(&ring_, &iov, 1);
    if (ret < 0) {
        return -1;
    }
    registered_buffers_.emplace_back(addr, len);
    return buffer_id;
}

bool IoUringBackend::unregister_buffer(int buffer_id) {
    // liburing doesn't support unregistering individual buffers
    (void)buffer_id;
    return false;
}

void IoUringBackend::async_send_fixed(int fd, int buffer_id, size_t offset, size_t len,
                                      ActorId actor, uint32_t op_type) {
    auto [buf_addr, buf_len] = registered_buffers_[buffer_id];
    struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    io_uring_prep_write_fixed(sqe, fd,
                              static_cast<const char*>(buf_addr) + offset,
                              len, 0, buffer_id);
    sqe->user_data = encode_user_data(fd, actor, op_type);
    pending_sqes_.push_back(sqe);
}

void IoUringBackend::async_recv_fixed(int fd, int buffer_id, size_t offset, size_t len,
                                      ActorId actor, uint32_t op_type) {
    auto [buf_addr, buf_len] = registered_buffers_[buffer_id];
    struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    io_uring_prep_read_fixed(sqe, fd,
                             static_cast<char*>(const_cast<void*>(buf_addr)) + offset,
                             len, 0, buffer_id);
    sqe->user_data = encode_user_data(fd, actor, op_type);
    pending_sqes_.push_back(sqe);
}

void IoUringBackend::async_accept(int fd, ActorId actor) {
    struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    io_uring_prep_accept(sqe, fd, nullptr, nullptr, 0);
    sqe->user_data = encode_user_data(fd, actor, static_cast<uint32_t>(OpType::Accept));
    pending_sqes_.push_back(sqe);
}

void IoUringBackend::async_connect(int fd, const sockaddr* addr, socklen_t addrlen,
                                   ActorId actor) {
    struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    io_uring_prep_connect(sqe, fd, addr, addrlen);
    sqe->user_data = encode_user_data(fd, actor, static_cast<uint32_t>(OpType::Connect));
    pending_sqes_.push_back(sqe);
}

void IoUringBackend::async_send(int fd, const iovec* bufs, int buf_count,
                                ActorId actor, uint32_t op_type) {
    struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    io_uring_prep_writev(sqe, fd, bufs, buf_count, 0);
    sqe->user_data = encode_user_data(fd, actor, op_type);
    pending_sqes_.push_back(sqe);
}

void IoUringBackend::async_recv(int fd, const iovec* bufs, int buf_count,
                                ActorId actor, uint32_t op_type) {
    struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    io_uring_prep_readv(sqe, fd, bufs, buf_count, 0);
    sqe->user_data = encode_user_data(fd, actor, op_type);
    pending_sqes_.push_back(sqe);
}

void IoUringBackend::async_recvfrom(int fd, const iovec* bufs, int buf_count,
                                     ActorId actor, uint32_t op_type) {
    // Build msghdr from iovec array
    struct msghdr msg{};
    msg.msg_iov = const_cast<struct iovec*>(bufs);
    msg.msg_iovlen = buf_count;

    struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    io_uring_prep_recvmsg(sqe, fd, &msg, 0);
    sqe->user_data = encode_user_data(fd, actor, op_type);
    pending_sqes_.push_back(sqe);
}

void IoUringBackend::async_sendto(int fd, const iovec* bufs, int buf_count,
                                  const sockaddr* addr, socklen_t addrlen,
                                  ActorId actor, uint32_t op_type) {
    // Build msghdr from iovec array
    struct msghdr msg{};
    msg.msg_name = const_cast<struct sockaddr*>(addr);
    msg.msg_namelen = addrlen;
    msg.msg_iov = const_cast<struct iovec*>(bufs);
    msg.msg_iovlen = buf_count;

    struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    io_uring_prep_sendmsg(sqe, fd, &msg, 0);
    sqe->user_data = encode_user_data(fd, actor, op_type);
    pending_sqes_.push_back(sqe);
}

uint64_t IoUringBackend::run_after(ActorId actor, int delay_ms) {
    uint64_t handle = next_timer_handle_++;
    struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    struct __kernel_timespec ts;
    ts.tv_sec = delay_ms / 1000;
    ts.tv_nsec = (delay_ms % 1000) * 1000000;
    io_uring_prep_timeout(sqe, &ts, 0, 0);
    sqe->user_data = encode_user_data(-1, actor, static_cast<uint32_t>(OpType::TimerFired));
    pending_sqes_.push_back(sqe);
    timer_actors_[handle] = actor;
    return handle;
}

uint64_t IoUringBackend::run_every(ActorId actor, int interval_ms) {
    // io_uring doesn't have native repeating timers, so treat as repeated run_after
    // For simplicity, just treat as a one-shot timer
    (void)interval_ms;
    return run_after(actor, interval_ms);
}

void IoUringBackend::cancel_timer(uint64_t handle) {
    cancelled_timers_.insert(handle);
    timer_actors_.erase(handle);
}

int IoUringBackend::submit() {
    if (pending_sqes_.empty()) {
        return 0;
    }
    int submitted = io_uring_submit(&ring_);
    if (submitted < 0) {
        std::abort(); // fatal
    }
    pending_sqes_.clear();
    return submitted;
}

int IoUringBackend::wait(int timeout_ms) {
    submit(); // submit pending before waiting
    struct io_uring_cqe* cqe = nullptr;
    int ret = io_uring_wait_cqe(&ring_, &cqe);
    if (ret < 0) {
        return ret;
    }
    return 1; // one CQE is ready
}

void IoUringBackend::process_completions() {
    struct io_uring_cqe* cqe = nullptr;
    while (io_uring_peek_cqe(&ring_, &cqe) == 0) {
        int fd;
        ActorId actor;
        uint32_t op_type;
        decode_user_data(cqe->user_data, fd, actor, op_type);

        OpCompletion completion{
            .actor = actor,
            .type = static_cast<OpType>(op_type),
            .fd = fd,
            .result = cqe->res,
            .user_data = cqe->user_data,
        };

        // Handle timer cancellation
        if (op_type == static_cast<uint32_t>(OpType::TimerFired)) {
            if (cancelled_timers_.count(cqe->user_data)) {
                cancelled_timers_.erase(cqe->user_data);
                io_uring_cqe_seen(&ring_, cqe);
                continue;
            }
        }

        deliver_completion(completion);
        io_uring_cqe_seen(&ring_, cqe);
    }
}

void IoUringBackend::deliver_completion(OpCompletion completion) {
    // Stub for now — actor system wiring is Phase 5.5
    (void)completion;
}

uint64_t IoUringBackend::encode_user_data(int fd, ActorId actor, uint32_t op_type) {
    return (static_cast<uint64_t>(fd) & 0xFFFFFFFFULL) |
           ((actor.value() & 0xFFFFULL) << 32) |
           ((static_cast<uint64_t>(op_type) & 0xFFULL) << 56);
}

void IoUringBackend::decode_user_data(uint64_t ud, int& fd, ActorId& actor, uint32_t& op_type) {
    fd = static_cast<int>(ud & 0xFFFFFFFFULL);
    actor = ActorId(static_cast<uint32_t>((ud >> 32) & 0xFFFFULL));
    op_type = static_cast<uint32_t>((ud >> 56) & 0xFFULL);
}

#else // !defined(__linux__)

// Stub implementations for macOS

IoUringBackend::IoUringBackend() : ring_{}, running_(false) {}

IoUringBackend::~IoUringBackend() {
    stop();
}

bool IoUringBackend::start() {
    running_ = true;
    return true;
}

void IoUringBackend::stop() {
    running_ = false;
}

bool IoUringBackend::add_fd(int fd, IoEvent events) {
    (void)fd;
    (void)events;
    return true;
}

bool IoUringBackend::update_fd(int fd, IoEvent events) {
    (void)fd;
    (void)events;
    return true;
}

bool IoUringBackend::remove_fd(int fd) {
    (void)fd;
    return true;
}

int IoUringBackend::register_buffer(const void* addr, size_t len) {
    (void)addr;
    (void)len;
    return -1;
}

bool IoUringBackend::unregister_buffer(int buffer_id) {
    (void)buffer_id;
    return false;
}

void IoUringBackend::async_send_fixed(int fd, int buffer_id, size_t offset, size_t len,
                                      ActorId actor, uint32_t op_type) {
    (void)fd;
    (void)buffer_id;
    (void)offset;
    (void)len;
    (void)actor;
    (void)op_type;
}

void IoUringBackend::async_recv_fixed(int fd, int buffer_id, size_t offset, size_t len,
                                      ActorId actor, uint32_t op_type) {
    (void)fd;
    (void)buffer_id;
    (void)offset;
    (void)len;
    (void)actor;
    (void)op_type;
}

void IoUringBackend::async_accept(int fd, ActorId actor) {
    (void)fd;
    (void)actor;
}

void IoUringBackend::async_connect(int fd, const sockaddr* addr, socklen_t addrlen,
                                   ActorId actor) {
    (void)fd;
    (void)addr;
    (void)addrlen;
    (void)actor;
}

void IoUringBackend::async_send(int fd, const iovec* bufs, int buf_count,
                                ActorId actor, uint32_t op_type) {
    (void)fd;
    (void)bufs;
    (void)buf_count;
    (void)actor;
    (void)op_type;
}

void IoUringBackend::async_recv(int fd, const iovec* bufs, int buf_count,
                                ActorId actor, uint32_t op_type) {
    (void)fd;
    (void)bufs;
    (void)buf_count;
    (void)actor;
    (void)op_type;
}

void IoUringBackend::async_recvfrom(int fd, const iovec* bufs, int buf_count,
                                     ActorId actor, uint32_t op_type) {
    (void)fd;
    (void)bufs;
    (void)buf_count;
    (void)actor;
    (void)op_type;
}

void IoUringBackend::async_sendto(int fd, const iovec* bufs, int buf_count,
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

uint64_t IoUringBackend::run_after(ActorId actor, int delay_ms) {
    (void)actor;
    (void)delay_ms;
    return 0;
}

uint64_t IoUringBackend::run_every(ActorId actor, int interval_ms) {
    (void)actor;
    (void)interval_ms;
    return 0;
}

void IoUringBackend::cancel_timer(uint64_t handle) {
    (void)handle;
}

int IoUringBackend::wait(int timeout_ms) {
    (void)timeout_ms;
    return 0;
}

void IoUringBackend::process_completions() {
}

void IoUringBackend::deliver_completion(OpCompletion completion) {
    (void)completion;
}

uint64_t IoUringBackend::encode_user_data(int fd, ActorId actor, uint32_t op_type) {
    return (static_cast<uint64_t>(fd) & 0xFFFFFFFFULL) |
           ((actor.value() & 0xFFFFULL) << 32) |
           ((static_cast<uint64_t>(op_type) & 0xFFULL) << 56);
}

void IoUringBackend::decode_user_data(uint64_t ud, int& fd, ActorId& actor, uint32_t& op_type) {
    fd = static_cast<int>(ud & 0xFFFFFFFFULL);
    actor = ActorId(static_cast<uint32_t>((ud >> 32) & 0xFFFFULL));
    op_type = static_cast<uint32_t>((ud >> 56) & 0xFFULL);
}

#endif // defined(__linux__)

} // namespace net
} // namespace hpactor
